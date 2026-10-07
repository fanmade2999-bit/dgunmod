// netmod_hook.cpp -- libnetmod.so <-> libapp_lib.so (ARM64) frame hook

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

#ifdef __ANDROID__
#include <android/log.h>
#define NM_LOG(...) __android_log_print(ANDROID_LOG_INFO, "netmod", __VA_ARGS__)
#else
#define NM_LOG(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#endif

struct _PWORK;

namespace off {
constexpr uintptr_t kLibPrgPLY = 0xc7c24;
constexpr uintptr_t kPrgTable = 0x12c408;
constexpr int       kKindPLY = 2;

constexpr uintptr_t kInUse = 0x00;
constexpr uintptr_t kFlags = 0x02;
constexpr uintptr_t kBody = 0x08;
constexpr uintptr_t kParam = 0x10;
constexpr uintptr_t kKind = 0x1a;
constexpr uintptr_t kBodyX = 0x0C;
constexpr uintptr_t kBodyY = 0x10;
constexpr uintptr_t kBodyZ = 0x14;
constexpr uintptr_t kParamAng = 0x04;
constexpr uint16_t kTeamMask = 0x00C0;
}

using prg_fn_t = void (*)(_PWORK*);
using pw_aloc_t = _PWORK* (*)(_PWORK*, int, int);
using pw_set_pos_t = _PWORK* (*)(_PWORK*, float, float, float, int);
using pw_free_t = void (*)(_PWORK*);

#include "netmod_api.h"

constexpr int kProxyTypeOverride = -1;
constexpr int kProxyAllocFlags = 0;
constexpr int kSetPosMode = 0;
constexpr float kSnapDistance = 50.0f;
constexpr float kSmoothTauSec = 0.06f;

namespace {
uintptr_t g_base = 0;
prg_fn_t* g_table_slot = nullptr;
prg_fn_t g_orig_prg = nullptr;
_PWORK** g_pwk_sym = nullptr;
pw_aloc_t g_pw_aloc = nullptr;
pw_set_pos_t g_pw_set_pos = nullptr;
pw_free_t g_pw_free = nullptr;

_PWORK* g_proxy = nullptr;
int16_t g_proxy_type = 0;
bool g_have_remote = false;
RemoteTransform g_target{};
float g_cur_x = 0, g_cur_y = 0, g_cur_z = 0, g_cur_ang = 0;
std::chrono::steady_clock::time_point g_last_tick;
std::atomic<bool> g_installed{false};

inline uint8_t* B(_PWORK* pw) { return reinterpret_cast<uint8_t*>(pw); }
template <class T> inline T& F(_PWORK* pw, uintptr_t o) { return *reinterpret_cast<T*>(B(pw) + o); }
inline uint8_t* body(_PWORK* pw) { return F<uint8_t*>(pw, off::kBody); }
inline uint8_t* param(_PWORK* pw) { return F<uint8_t*>(pw, off::kParam); }
inline bool slot_live(_PWORK* pw) { return pw && F<int16_t>(pw, off::kInUse) >= 1; }
inline _PWORK* local_player() { return g_pwk_sym ? *g_pwk_sym : nullptr; }

float wrap_pi(float a) {
    const float kPi = 3.14159265358979f;
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

// ---- proxy lifecycle -------------------------------------------------------
// A proxy gets kind (pw+0x1a) = 24, which targets an existing no-op ret program
// in the program table. This keeps the slot participating in normal targeting/spatial
// caches while ensuring chr_argo does not run gameplay logic for the proxy.
_PWORK* ensure_proxy(_PWORK* host) {
    if (g_proxy && F<int16_t>(g_proxy, off::kInUse) == g_proxy_type &&
        F<int8_t>(g_proxy, off::kKind) == 24)
        return g_proxy;
    g_proxy = nullptr;
    if (!g_pw_aloc) return nullptr;

    int type = (kProxyTypeOverride >= 1) ? kProxyTypeOverride
                                         : static_cast<int>(F<int16_t>(host, off::kInUse));
    if (type < 1 || type > 0x41) return nullptr;

    _PWORK* p = g_pw_aloc(nullptr, type, kProxyAllocFlags);
    if (!p) { 
        NM_LOG("pw_aloc(type %d) returned null (pool full / type not loaded)", type); 
        return nullptr; 
    }

    // Kind 24 = index of existing no-op 'ret' program in 0x12c408 table
    F<int8_t>(p, off::kKind) = 24;
    g_proxy = p;
    g_proxy_type = static_cast<int16_t>(type);
    g_have_remote = false;
    return g_proxy;
}

constexpr bool kPvP = false;

void apply_team_flags(_PWORK* proxy, _PWORK* host) {
    uint16_t& pf = F<uint16_t>(proxy, off::kFlags);
    if (kPvP) {
        pf = static_cast<uint16_t>((pf & ~0x00E0) | 0x0080);
    } else {
        const uint16_t hf = F<uint16_t>(host, off::kFlags);
        pf = static_cast<uint16_t>((pf & ~off::kTeamMask) | (hf & off::kTeamMask));
    }
}

void sync_remote(_PWORK* host, float dt) {
    RemoteTransform rt;
    if (netmod_get_remote_transform(&rt)) {
        g_target = rt;
        if (!g_have_remote) {
            g_cur_x = rt.x; g_cur_y = rt.y; g_cur_z = rt.z; g_cur_ang = rt.angle; g_have_remote = true;
        }
    }
    if (!g_have_remote) return;

    _PWORK* proxy = ensure_proxy(host);
    if (!proxy || !g_pw_set_pos) return;

    const float dx = g_target.x - g_cur_x, dy = g_target.y - g_cur_y, dz = g_target.z - g_cur_z;
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > kSnapDistance) {
        g_cur_x = g_target.x; g_cur_y = g_target.y; g_cur_z = g_target.z;
    } else {
        const float a = 1.0f - std::exp(-dt / kSmoothTauSec);
        g_cur_x += dx * a; g_cur_y += dy * a; g_cur_z += dz * a;
    }
    g_cur_ang = wrap_pi(g_cur_ang + wrap_pi(g_target.angle - g_cur_ang) *
                        (1.0f - std::exp(-dt / kSmoothTauSec)));

    g_pw_set_pos(proxy, g_cur_x, g_cur_y, g_cur_z, kSetPosMode);

    if (uint8_t* prm = param(proxy)) *reinterpret_cast<float*>(prm + off::kParamAng) = g_cur_ang;
    apply_team_flags(proxy, host);
}

void hk_prg_PLY(_PWORK* pw) {
    if (pw != local_player()) { g_orig_prg(pw); return; }

    g_orig_prg(pw);

    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_last_tick).count();
    g_last_tick = now;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;

    uint8_t* bd = body(pw);
    uint8_t* pr = param(pw);
    if (bd && pr) {
        netmod_update_local_transform(*reinterpret_cast<float*>(bd + off::kBodyX),
                                      *reinterpret_cast<float*>(bd + off::kBodyY),
                                      *reinterpret_cast<float*>(bd + off::kBodyZ),
                                      *reinterpret_cast<float*>(pr + off::kParamAng));
    }

    sync_remote(pw, dt);
}
}

extern "C" bool netmod_hook_install() {
    if (g_installed.load()) return true;

    void* h = dlopen("libapp_lib.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) { NM_LOG("libapp_lib.so not loaded yet"); return false; }

    void* prg = dlsym(h, "_Z7prg_PLYP6_PWORK");
    g_pwk_sym = static_cast<_PWORK**>(dlsym(h, "pwk"));
    g_pw_aloc = reinterpret_cast<pw_aloc_t>(dlsym(h, "_Z7pw_alocP6_PWORKii"));
    g_pw_set_pos = reinterpret_cast<pw_set_pos_t>(dlsym(h, "_Z10pw_set_posP6_PWORKfffi"));
    g_pw_free = reinterpret_cast<pw_free_t>(dlsym(h, "_Z7pw_freeP6_PWORK"));
    if (!prg || !g_pwk_sym || !g_pw_set_pos) { NM_LOG("required symbols missing"); return false; }

    g_base = reinterpret_cast<uintptr_t>(prg) - off::kLibPrgPLY;
    g_table_slot = reinterpret_cast<prg_fn_t*>(g_base + off::kPrgTable) + off::kKindPLY;

    if (*g_table_slot != reinterpret_cast<prg_fn_t>(prg)) {
        NM_LOG("table[2]=%p != prg_PLY=%p -- wrong library build, not patching",
               reinterpret_cast<void*>(*g_table_slot), prg);
        return false;
    }

    const long pg = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(g_table_slot) & ~(pg - 1));
    if (mprotect(page, pg, PROT_READ | PROT_WRITE) != 0) { NM_LOG("mprotect failed"); return false; }

    g_orig_prg = *g_table_slot;
    g_last_tick = std::chrono::steady_clock::now();
    __atomic_store_n(reinterpret_cast<void**>(g_table_slot), reinterpret_cast<void*>(&hk_prg_PLY),
                     __ATOMIC_RELEASE);

    g_installed.store(true);
    NM_LOG("hooked table[2] @ %p (base %p)", static_cast<void*>(g_table_slot),
           reinterpret_cast<void*>(g_base));
    return true;
}

extern "C" void netmod_hook_uninstall() {
    if (!g_installed.exchange(false)) return;
    __atomic_store_n(reinterpret_cast<void**>(g_table_slot), reinterpret_cast<void*>(g_orig_prg),
                     __ATOMIC_RELEASE);
    if (slot_live(g_proxy) && g_pw_free) g_pw_free(g_proxy);
    g_proxy = nullptr;
}
