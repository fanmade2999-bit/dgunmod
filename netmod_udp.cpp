// netmod_udp.cpp -- non-blocking-ish UDP transport for netmod.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include "netmod_api.h"

#ifdef __ANDROID__
#include <android/log.h>
#define NM_LOG(...) __android_log_print(ANDROID_LOG_INFO, "netmod_udp", __VA_ARGS__)
#else
#define NM_LOG(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#endif

namespace {
constexpr uint16_t kPort = 8888;
constexpr uint16_t kMagic = 0x4D4E;
constexpr uint8_t kVersion = 1;
constexpr auto kSendEvery = std::chrono::milliseconds(33);
constexpr float kMaxCoord = 1.0e6f;

#pragma pack(push, 1)
struct Packet {
    uint16_t magic;
    uint8_t version;
    uint8_t reserved;
    uint32_t sender_id;
    uint32_t seq;
    float x, y, z, angle;
};
#pragma pack(pop)
static_assert(sizeof(Packet) == 28, "wire format changed");

std::atomic<int> g_fd{-1};
std::atomic<bool> g_running{false};
std::thread g_rx;
sockaddr_in g_peer{};
std::mutex g_peer_mx;
uint32_t g_self_id = 0;
uint32_t g_tx_seq = 0;
std::chrono::steady_clock::time_point g_last_tx{};

std::mutex g_rx_mx;
RemoteTransform g_rx_latest{};
uint32_t g_rx_sender = 0;
bool g_rx_new = false;

bool sane(float v) { return std::isfinite(v) && std::fabs(v) < kMaxCoord; }

void rx_loop(int fd) {
    Packet p;
    while (g_running.load()) {
        ssize_t n = recvfrom(fd, &p, sizeof(p), 0, nullptr, nullptr);
        if (n < 0) continue;  // SO_RCVTIMEO/interrupt; no packet to diagnose.
        NM_LOG("UDP rx: len=%zd", n);
        if (n != static_cast<ssize_t>(sizeof(p))) {
            NM_LOG("UDP drop: invalid len %zd (expected %zu)", n, sizeof(p));
            continue;
        }
        NM_LOG("UDP rx header: magic=%04x version=%u sender=%08x seq=%u",
               p.magic, p.version, p.sender_id, p.seq);
        if (p.magic != kMagic) {
            NM_LOG("UDP drop: bad magic %04x (expected %04x)", p.magic, kMagic);
            continue;
        }
        if (p.version != kVersion) {
            NM_LOG("UDP drop: bad version %u (expected %u)", p.version, kVersion);
            continue;
        }
        if (p.sender_id == g_self_id) {
            NM_LOG("UDP drop: self sender %08x", p.sender_id);
            continue;
        }
        if (!sane(p.x) || !sane(p.y) || !sane(p.z) || !std::isfinite(p.angle)) {
            NM_LOG("UDP drop: invalid transform x=%g y=%g z=%g angle=%g",
                   p.x, p.y, p.z, p.angle);
            continue;
        }

        std::lock_guard<std::mutex> lk(g_rx_mx);
        const bool newer = p.sender_id != g_rx_sender ||
                           static_cast<int32_t>(p.seq - g_rx_latest.seq) > 0;
        if (!newer) {
            NM_LOG("UDP drop: stale seq %u from sender %08x (latest %u)",
                   p.seq, p.sender_id, g_rx_latest.seq);
            continue;
        }
        g_rx_sender = p.sender_id;
        g_rx_latest = {p.x, p.y, p.z, p.angle, p.seq};
        g_rx_new = true;
    }
}
}

extern "C" void netmod_set_peer(const char* ip) {
    in_addr a{};
    if (!ip || inet_pton(AF_INET, ip, &a) != 1) { NM_LOG("bad peer address"); return; }
    std::lock_guard<std::mutex> lk(g_peer_mx);
    g_peer.sin_addr = a;
}

extern "C" void netmod_net_start() {
    if (g_running.exchange(true)) return;

    g_self_id = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                (static_cast<uint32_t>(getpid()) * 2654435761u);
    if (g_self_id == 0) g_self_id = 1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { NM_LOG("socket() failed"); g_running = false; return; }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    timeval tv{0, 200 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        NM_LOG("bind(%u) failed", kPort);
        close(fd);
        g_running = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_peer_mx);
        g_peer = {};
        g_peer.sin_family = AF_INET;
        g_peer.sin_port = htons(kPort);
        inet_pton(AF_INET, "192.168.43.255", &g_peer.sin_addr);
    }

    g_fd.store(fd);
    g_rx = std::thread(rx_loop, fd);
    NM_LOG("UDP transport on port %u (self id %08x)", kPort, g_self_id);
}

extern "C" void netmod_net_stop() {
    if (!g_running.exchange(false)) return;
    int fd = g_fd.load();
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    if (g_rx.joinable()) g_rx.join();
    fd = g_fd.exchange(-1);
    if (fd >= 0) close(fd);
}

extern "C" void netmod_update_local_transform(float x, float y, float z, float angle) {
    const int fd = g_fd.load();
    if (fd < 0) return;

    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_tx < kSendEvery) return;
    g_last_tx = now;

    Packet p{};
    p.magic = kMagic;
    p.version = kVersion;
    p.sender_id = g_self_id;
    p.seq = ++g_tx_seq;
    p.x = x; p.y = y; p.z = z; p.angle = angle;

    sockaddr_in dst;
    { std::lock_guard<std::mutex> lk(g_peer_mx); dst = g_peer; }
    sendto(fd, &p, sizeof(p), MSG_DONTWAIT,
           reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}

extern "C" bool netmod_get_remote_transform(RemoteTransform* out) {
    std::lock_guard<std::mutex> lk(g_rx_mx);
    if (!g_rx_new) return false;
    if (out) *out = g_rx_latest;
    g_rx_new = false;
    return true;
}
