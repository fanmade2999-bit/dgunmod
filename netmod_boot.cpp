// netmod_boot.cpp -- loaded via System.loadLibrary("netmod") from the patched APK.
// JNI_OnLoad -> netmod_init() -> background thread:
//   1. start the UDP transport (your code, optional weak symbol)
//   2. wait until libapp_lib.so is mapped, then install the prg_PLY table hook.
// The game loads libapp_lib.so on its own schedule, so we poll instead of assuming order.

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <thread>

#ifdef __ANDROID__
#include <android/log.h>
#define NM_LOG(...) __android_log_print(ANDROID_LOG_INFO, "netmod", __VA_ARGS__)
#else
#define NM_LOG(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#endif

#define NM_EXPORT extern "C" __attribute__((visibility("default")))

extern "C" bool netmod_hook_install();                       // netmod_hook.cpp
extern "C" void netmod_net_start();                          // netmod_udp.cpp

namespace {
std::atomic<bool> g_started{false};

void bootstrap_thread() {
    netmod_net_start();

    // Wait up to ~60 s for the game library.
    for (int i = 0; i < 300; ++i) {
        void* h = dlopen("libapp_lib.so", RTLD_NOW | RTLD_NOLOAD);
        if (h && dlsym(h, "_Z7prg_PLYP6_PWORK")) {
            // Library is mapped: try a few times in case of a transient failure,
            // then give up (a mismatch means a different build -- retrying won't help).
            for (int t = 0; t < 5; ++t) {
                if (netmod_hook_install()) { NM_LOG("hook installed"); return; }
                usleep(200 * 1000);
            }
            NM_LOG("hook install failed; giving up");
            return;
        }
        usleep(200 * 1000);
    }
    NM_LOG("libapp_lib.so never appeared; hook not installed");
}
}  // namespace

NM_EXPORT void netmod_init() {
    if (g_started.exchange(true)) return;
    std::thread(bootstrap_thread).detach();
}

// Called by the VM on System.loadLibrary("netmod"). Signature kept free of jni.h.
NM_EXPORT int JNI_OnLoad(void* /*JavaVM*/, void* /*reserved*/) {
    netmod_init();
    return 0x00010006;  // JNI_VERSION_1_6
}
