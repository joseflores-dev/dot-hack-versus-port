/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <csignal>
#include <cstdio>
#include <cstring>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

extern "C" void ppu_dump_all_guest_stacks(const char* tag);

/* `kill -USR2 <pid>` from adb: every guest thread's stack, for hangs where all
 * host threads sit in futex waits and nothing logs. Not async-signal-safe --
 * a debugging aid only. */
static void on_sigusr2(int)
{
    std::fprintf(stderr, "[SIGUSR2] guest stacks:\n");
    ppu_dump_all_guest_stacks("SIGUSR2");
}

extern "C" void hv_prof_start(void);

extern "C" void ps3_load_prx_modules(void)
{
#ifndef _WIN32
    std::signal(SIGUSR2, on_sigusr2);
#endif
    hv_prof_start();   /* HV_PROF=<seconds> */
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */

/* ---------------------------------------------------------------------------
 * Android on-screen pad (APK build)
 *
 * PadView (Java) draws the PS3 controls over the game and reports the pressed set here. It feeds an SDL virtual
 * joystick typed as a game controller, so cellPad's SDL2 backend opens it like a real pad and the title reads it
 * through the ordinary cellPadGetData path. A Bluetooth gamepad keeps working next to it.
 * -----------------------------------------------------------------------*/
#ifdef HV_APK
#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <SDL2/SDL.h>

static SDL_Joystick* s_vpad;          /* no longer opened: the touches go straight into cellPad's port 0 */
static volatile int s_vpad_touched;   /* the player pressed something: the intro skipper stops */
/* What the on-screen pad holds down, as SDL_GameControllerButton bits (16/17 = L2/R2); cellPad ORs it into
 * port 0 with a real gamepad's state, so the touch pad is always player 1 and never takes a port of its own. */
volatile unsigned g_hv_touch_buttons;
volatile int      g_hv_touch_present;

/* Intro skipper (HV_AUTOSKIP=0 turns it off): the title opens on its
 * Stereoscopic settings screen after six skippable screens (the fan patch's
 * note, three logos, a disclaimer). Press X on the virtual pad until the
 * Vulkan backend sees that screen in a presented frame (hv_autoskip_done),
 * then stop: one X more there would open the setting. Gives up after 90 s. */
extern "C" volatile int hv_autoskip_watch;
extern "C" volatile int hv_autoskip_done;

static void* hv_autoskip(void*)
{
    hv_autoskip_watch = 1;
    int presses = 0;
    for (int t = 0; t < 900 && !hv_autoskip_done && !s_vpad_touched; t++) {
        usleep(100000);
        if (t < 40 || t % 15) continue;            /* 4 s of boot, then one press every 1.5 s */
        if (hv_autoskip_done || s_vpad_touched) break;
        g_hv_touch_buttons |= 1u << SDL_CONTROLLER_BUTTON_A;
        usleep(150000);
        g_hv_touch_buttons &= ~(1u << SDL_CONTROLLER_BUTTON_A);
        presses++;
    }
    hv_autoskip_watch = 0;
    std::fprintf(stderr, "[vpad] intro skip: %s after %d presses\n",
                 hv_autoskip_done ? "settings screen reached" : s_vpad_touched ? "player took over" : "gave up",
                 presses);
    return nullptr;
}

/* Called once from main.cpp's apk_prepare, on the SDL main thread, before the title runs. */
extern "C" void hv_vpad_init(void)
{
    /* A Bluetooth DualShock 4 ("Wireless Controller") was opened by SDL's HIDAPI driver as "PS4 Controller" and
     * then never reported a button: on Android that driver only reads USB and BLE devices. Without it SDL takes the
     * pad through Android's own input path, with the standard DS4 mapping. HV_HIDAPI=1 keeps the driver. */
    if (!(std::getenv("HV_HIDAPI") && std::getenv("HV_HIDAPI")[0] == '1')) SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[vpad] SDL joystick init failed: %s\n", SDL_GetError());
        return;
    }
    s_vpad = nullptr;
    g_hv_touch_present = 1;
    std::fprintf(stderr, "[vpad] on-screen pad feeds port 0 directly\n");
    const char* skip = std::getenv("HV_AUTOSKIP");
    if (skip && skip[0] == '1') {
        pthread_t th;
        if (pthread_create(&th, nullptr, hv_autoskip, nullptr) == 0) pthread_detach(th);
    }
}

/* PadView.nativePad(int buttons): bit n = SDL_GameControllerButton n; bits 16/17 = L2/R2 (trigger axes). */
extern "C" JNIEXPORT void JNICALL Java_com_hackversus_port_PadView_nativePad(JNIEnv*, jclass, jint buttons)
{
    static int logged = 0;
    if (logged < 12) { logged++; std::fprintf(stderr, "[vpad] buttons 0x%05X%s\n", (unsigned)buttons, s_vpad ? "" : " (no pad)"); }
    if (buttons) s_vpad_touched = 1;
    g_hv_touch_buttons = (unsigned)buttons;
}
#endif

/* ---------------------------------------------------------------------------
 * HV_PROF=<seconds>: a sampling profiler for a device with no simpleperf or
 * debuggerd access. A host thread sends SIGPROF to every thread of the process
 * once a millisecond; the handler records the interrupted PC. When the time is
 * up the hottest addresses go to stderr as "<module> +<offset> <samples>",
 * which llvm-symbolizer resolves against the unstripped library.
 * -----------------------------------------------------------------------*/
#if !defined(_WIN32)
#include <atomic>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <algorithm>
#include <vector>

static const int kProfMax = 1 << 20;
static uintptr_t* s_prof_pc;
static std::atomic<int> s_prof_n{0};

/* libc's text range: a sample inside it is charged to the first caller outside, found through the frame-pointer
 * chain (x29) -- "something is in futex/nanosleep" says nothing, who called it does. */
static uintptr_t s_libc_lo, s_libc_hi;

static void on_sigprof(int, siginfo_t*, void* uc)
{
    const mcontext_t& m = ((ucontext_t*)uc)->uc_mcontext;
    uintptr_t pc = (uintptr_t)m.pc;
    if (pc >= s_libc_lo && pc < s_libc_hi) {
        uintptr_t lr = (uintptr_t)m.regs[30], fp = (uintptr_t)m.regs[29];
        pc = lr & 0x7FFFFFFFFFull;
        for (int k = 0; k < 8 && pc >= s_libc_lo && pc < s_libc_hi && fp && !(fp & 7); k++) {
            pc = ((uintptr_t*)fp)[1] & 0x7FFFFFFFFFull;   /* strip the PAC signature */
            fp = ((uintptr_t*)fp)[0];
        }
    }
    /* HV_PROF_UP=<n>: charge the sample n frames up instead -- who called the lock wait. */
    static int up = -1;
    if (up < 0) { const char* e = getenv("HV_PROF_UP"); up = e ? atoi(e) : 0; }
    if (up > 0) {
        uintptr_t fp = (uintptr_t)m.regs[29];
        for (int k = 0; k < up && fp && !(fp & 7); k++) {
            pc = ((uintptr_t*)fp)[1] & 0x7FFFFFFFFFull;
            fp = ((uintptr_t*)fp)[0];
        }
    }
    const int i = s_prof_n.fetch_add(1, std::memory_order_relaxed);
    /* The thread in the top bits (a PC without its PAC signature fits in 39): the report is per thread. */
    if (i < kProfMax) s_prof_pc[i] = (pc & 0x7FFFFFFFFFull) | ((uintptr_t)syscall(SYS_gettid) << 40);
}

static void* prof_thread(void* arg)
{
    const long packed = (long)(intptr_t)arg, secs = packed & 0xFFFF;   /* HV_PROF=[delay:]seconds */
    sleep((unsigned)(packed >> 16));
    const pid_t self = (pid_t)syscall(SYS_gettid), pid = getpid();
    for (long ms = 0; ms < secs * 1000; ms++) {
        if (DIR* d = opendir("/proc/self/task")) {
            while (dirent* e = readdir(d)) {
                const int tid = atoi(e->d_name);
                if (tid <= 0 || tid == self) continue;
                /* Only threads on a CPU: the parked ones would fill the buffer with futex waits. */
                char path[64], st[256] = {0};
                snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
                if (FILE* f = fopen(path, "r")) { fread(st, 1, sizeof st - 1, f); fclose(f); }
                const char* rp = strrchr(st, ')');
                if (rp && rp[1] == ' ' && rp[2] == 'R') syscall(SYS_tgkill, pid, tid, SIGPROF);
            }
            closedir(d);
        }
        usleep(1000);
    }
    const int n = std::min(s_prof_n.load(), kProfMax);
    std::vector<uintptr_t> v(s_prof_pc, s_prof_pc + n);
    std::sort(v.begin(), v.end());
    std::vector<std::pair<int, uintptr_t>> hot;
    for (size_t i = 0; i < v.size();) {
        size_t j = i; while (j < v.size() && v[j] == v[i]) j++;
        hot.push_back({(int)(j - i), v[i]}); i = j;
    }
    std::sort(hot.rbegin(), hot.rend());
    std::fprintf(stderr, "[prof] %d samples over %lds\n", n, secs);
    /* Per thread: its total, then its hottest addresses, for the busiest eight threads. */
    std::vector<std::pair<int, long>> tids;
    for (auto& h : hot) {
        const long tid = (long)(h.second >> 40);
        auto it = std::find_if(tids.begin(), tids.end(),
                               [&](const std::pair<int, long>& t) { return t.second == tid; });
        if (it == tids.end()) tids.push_back({h.first, tid}); else it->first += h.first;
    }
    std::sort(tids.rbegin(), tids.rend());
    for (size_t t = 0; t < tids.size() && t < 12; t++) {
        std::fprintf(stderr, "[prof] thread %ld: %d samples\n", tids[t].second, tids[t].first);
        int shown = 0;
        for (size_t k = 0; k < hot.size() && shown < 400; k++) {
            if ((long)(hot[k].second >> 40) != tids[t].second) continue;
            shown++;
            const uintptr_t pc = hot[k].second & 0xFFFFFFFFFFull;
            Dl_info di;
            if (dladdr((void*)pc, &di) && di.dli_fname)
                std::fprintf(stderr, "[prof] %s +0x%lx %d\n", di.dli_fname,
                             (unsigned long)(pc - (uintptr_t)di.dli_fbase), hot[k].first);
            else
                std::fprintf(stderr, "[prof] ? 0x%lx %d\n", (unsigned long)pc, hot[k].first);
        }
    }
    std::fflush(stderr);
    return nullptr;
}

/* HV_STACKS_EVERY=<seconds>: every guest thread's stack to the log on a timer -- the app's process cannot be
 * sent SIGUSR2 from adb, so this is how a stall inside the APK gets located. */
static void* stacks_thread(void* arg)
{
    const unsigned secs = (unsigned)(uintptr_t)arg;
    for (unsigned n = 0; n < 6; n++) {
        sleep(secs);
        std::fprintf(stderr, "[stacks] dump %u\n", n);
        ppu_dump_all_guest_stacks("STACKS");
    }
    return nullptr;
}

/* HV_UCLAMP=<0..1024>: utilization clamp (sched_setattr SCHED_FLAG_UTIL_CLAMP_MIN) on
 * every thread of the process, re-applied every 2 s for threads created since.
 * In a fight no thread is ever 100 % busy -- each one waits on another -- so the
 * walt governor settles at 1.2-1.6 GHz on cores that can do 2.5-3.0; a clamp
 * tells it the threads want speed regardless of their measured utilization. */
#include <cerrno>
struct hv_sched_attr {
    uint32_t size; uint32_t sched_policy; uint64_t sched_flags; int32_t sched_nice;
    uint32_t sched_priority; uint64_t sched_runtime, sched_deadline, sched_period;
    uint32_t sched_util_min, sched_util_max;
};
static void* hv_uclamp_thread(void* arg)
{
    const unsigned clamp = (unsigned)(uintptr_t)arg;
    int applied = 0, failed = 0;
    for (int round = 0;; round++) {
        if (DIR* d = opendir("/proc/self/task")) {
            while (dirent* e = readdir(d)) {
                const int tid = atoi(e->d_name);
                if (tid <= 0) continue;
                hv_sched_attr a; memset(&a, 0, sizeof a);
                a.size = sizeof a;
                a.sched_flags = 0x08 | 0x10 | 0x20 | 0x40;   /* KEEP_POLICY | KEEP_PARAMS | UTIL_CLAMP_MIN | UTIL_CLAMP_MAX */
                a.sched_util_min = clamp; a.sched_util_max = 1024;
                if (syscall(274 /* sched_setattr */, tid, &a, 0) == 0) applied++; else failed++;
            }
            closedir(d);
        }
        if (round == 0 || round % 30 == 0)
            std::fprintf(stderr, "[uclamp] min=%u: %d threads set, %d failed (errno %d)\n", clamp, applied, failed, errno);
        applied = failed = 0;
        sleep(2);
    }
    return nullptr;
}

/* HV_ADPF=1: an Android performance-hint session (ADPF, libandroid.so API 33) over
 * the process's eight busiest threads, re-chosen every 5 s, with a 33.3 ms target
 * and the real frame time reported from the Vulkan present (hv_adpf_frame). It is
 * the sanctioned way for an app to ask for CPU speed; sched_setattr's uclamp is
 * EPERM for an app on this kernel. */
#include <dlfcn.h>
typedef void* (*hv_fn_get_manager)(void);
typedef void* (*hv_fn_create_session)(void*, const int32_t*, size_t, int64_t);
typedef int   (*hv_fn_report)(void*, int64_t);
typedef void  (*hv_fn_close)(void*);
typedef int   (*hv_fn_set_threads)(void*, const int32_t*, size_t);
static void*        s_adpf_session;
static hv_fn_report s_adpf_report;
static int64_t      s_adpf_last_ns;
static int64_t hv_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec; }
extern "C" void hv_adpf_frame(void)
{
    if (!s_adpf_session || !s_adpf_report) return;
    const int64_t now = hv_now_ns();
    if (s_adpf_last_ns) s_adpf_report(s_adpf_session, now - s_adpf_last_ns);
    s_adpf_last_ns = now;
}
static void* hv_adpf_thread(void* arg)
{
    const int64_t target_ns = (int64_t)(intptr_t)arg;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    hv_fn_get_manager get_manager = lib ? (hv_fn_get_manager)dlsym(lib, "APerformanceHint_getManager") : nullptr;
    hv_fn_create_session create = lib ? (hv_fn_create_session)dlsym(lib, "APerformanceHint_createSession") : nullptr;
    hv_fn_report report = lib ? (hv_fn_report)dlsym(lib, "APerformanceHint_reportActualWorkDuration") : nullptr;
    hv_fn_close close_s = lib ? (hv_fn_close)dlsym(lib, "APerformanceHint_closeSession") : nullptr;
    hv_fn_set_threads set_threads = lib ? (hv_fn_set_threads)dlsym(lib, "APerformanceHint_setThreads") : nullptr;
    void* mgr = get_manager ? get_manager() : nullptr;
    if (!mgr || !create || !report) { std::fprintf(stderr, "[adpf] unavailable (manager %p create %p report %p)\n", mgr, (void*)create, (void*)report); return nullptr; }
    std::fprintf(stderr, "[adpf] manager ready, target %.1f ms, setThreads %s\n", target_ns / 1e6, set_threads ? "yes" : "no");
    static long long prev[4096]; int32_t chosen[8]; int n_prev_chosen = 0; int32_t prev_chosen[8] = {0};
    for (int round = 0;; round++) {
        /* the busiest threads since the last round, by utime+stime delta */
        struct { int tid; long long d; } all[256], top[8]; int n_all = 0, n = 0;
        if (DIR* d = opendir("/proc/self/task")) {
            while (dirent* e = readdir(d)) {
                const int tid = atoi(e->d_name); if (tid <= 0) continue;
                char path[64], st[512] = {0}; snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
                FILE* f = fopen(path, "r"); if (!f) continue; fread(st, 1, sizeof st - 1, f); fclose(f);
                const char* rp = strrchr(st, ')'); if (!rp) continue;
                long long u = 0, s = 0; int k = 0; const char* p = rp + 2;   /* field 3 onwards */
                for (; *p && k < 11; p++) if (*p == ' ') k++;                /* skip to field 14 (utime) */
                sscanf(p, "%lld %lld", &u, &s);
                const long long now = u + s, delta = now - prev[tid & 4095]; prev[tid & 4095] = now;
                if (round == 0 || delta <= 0 || n_all >= 256) continue;
                all[n_all].tid = tid; all[n_all].d = delta; n_all++;
            }
            closedir(d);
        }
        for (int i = 1; i < n_all; i++)   /* insertion sort, busiest first */
            for (int j = i; j > 0 && all[j].d > all[j - 1].d; j--) { auto t = all[j]; all[j] = all[j - 1]; all[j - 1] = t; }
        for (n = 0; n < 8 && n < n_all; n++) top[n] = all[n];
        if (n) {
            for (int i = 0; i < n; i++) chosen[i] = top[i].tid;
            int same = (n == n_prev_chosen); for (int i = 0; same && i < n; i++) same = chosen[i] == prev_chosen[i];
            if (!same) {
                if (s_adpf_session && set_threads) { set_threads(s_adpf_session, chosen, (size_t)n); }
                else {
                    void* old = s_adpf_session; s_adpf_session = nullptr;
                    if (old && close_s) close_s(old);
                    s_adpf_session = create(mgr, chosen, (size_t)n, target_ns); s_adpf_report = report; s_adpf_last_ns = 0;
                }
                std::fprintf(stderr, "[adpf] session %p over %d threads:", s_adpf_session, n);
                for (int i = 0; i < n; i++) std::fprintf(stderr, " %d(%lld)", chosen[i], top[i].d);
                std::fprintf(stderr, "\n");
                memcpy(prev_chosen, chosen, sizeof chosen); n_prev_chosen = n;
            }
        }
        sleep(round == 0 ? 2 : 5);
    }
    return nullptr;
}

extern "C" void hv_prof_start(void)
{
    if (const char* ad = getenv("HV_ADPF")) {
        pthread_t t;
        const long target_us = atol(ad) > 1 ? atol(ad) : 33333;   /* HV_ADPF=<target us> or 1 */
        if (pthread_create(&t, nullptr, hv_adpf_thread, (void*)(intptr_t)(target_us * 1000)) == 0) pthread_detach(t);
    }
    if (const char* uc = getenv("HV_UCLAMP")) {
        pthread_t t;
        if (pthread_create(&t, nullptr, hv_uclamp_thread, (void*)(uintptr_t)atoi(uc)) == 0) pthread_detach(t);
    }
    if (const char* se = getenv("HV_STACKS_EVERY")) {
        pthread_t t;
        pthread_create(&t, nullptr, stacks_thread, (void*)(uintptr_t)atoi(se));
        pthread_detach(t);
    }
    const char* e = getenv("HV_PROF");
    if (!e || atoi(e) <= 0) return;
    s_prof_pc = (uintptr_t*)calloc(kProfMax, sizeof(uintptr_t));
    {   /* libc's mapped range, from its own symbols' module base and /proc/self/maps */
        Dl_info di;
        if (dladdr((void*)&nanosleep, &di) && di.dli_fbase) {
            s_libc_lo = (uintptr_t)di.dli_fbase;
            s_libc_hi = s_libc_lo;
            if (FILE* f = fopen("/proc/self/maps", "r")) {
                char line[512];
                while (fgets(line, sizeof line, f)) {
                    unsigned long lo, hi;
                    if (strstr(line, "/libc.so") && sscanf(line, "%lx-%lx", &lo, &hi) == 2 && hi > s_libc_hi)
                        s_libc_hi = hi;
                }
                fclose(f);
            }
        }
    }
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigprof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    pthread_t t;
    const char* colon = strchr(e, ':');
    const long delay = colon ? atol(e) : 0, secs = colon ? atol(colon + 1) : atol(e);
    pthread_create(&t, nullptr, prof_thread, (void*)(intptr_t)((delay << 16) | (secs & 0xFFFF)));
    pthread_detach(t);
    std::fprintf(stderr, "[prof] sampling every 1 ms for %ss\n", e);
}
#endif

/* ---------------------------------------------------------------------------
 * HV_PROF=[delay:]seconds on Windows (the PC build): every millisecond each other thread of the process is
 * suspended and its RIP read. At the end, per thread (with its name), the hottest addresses go to stderr as
 * "[prof] <tid> <name> +<rva> <samples>"; llvm-symbolizer resolves the RVAs against the unstripped .exe.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
static DWORD WINAPI hv_prof_thread_win(LPVOID arg)
{
    const long packed = (long)(intptr_t)arg, secs = packed & 0xFFFF, delay = packed >> 16;
    Sleep((DWORD)delay * 1000);
    /* HV_PROF_GO=<file>: start only once that file exists (a script creates it when the fight is on screen). */
    if (const char* go = getenv("HV_PROF_GO"))
        while (GetFileAttributesA(go) == INVALID_FILE_ATTRIBUTES) Sleep(100);
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::map<DWORD, std::map<uintptr_t, unsigned>> hist;
    std::map<DWORD, unsigned> total;
    std::fprintf(stderr, "[prof] sampling every 1 ms for %lds\n", secs);
    const ULONGLONG end = GetTickCount64() + (ULONGLONG)secs * 1000;
    std::vector<std::pair<DWORD, HANDLE>> th;
    th.reserve(512);
    struct Samp { DWORD tid; uintptr_t pc; };
    const size_t kMax = 4u << 20;
    Samp* samp = (Samp*)malloc(kMax * sizeof(Samp));
    size_t n_samp = 0;
    ULONGLONG refresh = 0;
    while (GetTickCount64() < end) {
        if (GetTickCount64() >= refresh) {   /* the thread list, once a second */
            for (auto& t : th) CloseHandle(t.second);
            th.clear();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te; te.dwSize = sizeof te;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (h) th.push_back({te.th32ThreadID, h});
            }
            CloseHandle(snap);
            refresh = GetTickCount64() + 1000;
        }
        /* Nothing may allocate while a thread is suspended: it can be holding the heap lock. */
        for (auto& t : th) {
            if (n_samp >= kMax) break;
            if (SuspendThread(t.second) == (DWORD)-1) continue;
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            const BOOL ok = GetThreadContext(t.second, &c);
            ResumeThread(t.second);
            if (ok) samp[n_samp++] = { t.first, (uintptr_t)c.Rip };
        }
        Sleep(1);
    }
    for (size_t i = 0; i < n_samp; i++) { hist[samp[i].tid][samp[i].pc]++; total[samp[i].tid]++; }
    free(samp);
    for (auto& t : hist) {
        if (total[t.first] < 50) continue;
        unsigned in_exe = 0;
        for (auto& a : t.second) if (a.first >= base && a.first < base + 0x10000000) in_exe += a.second;
        std::string name = "?";
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, t.first);
        PWSTR wn = nullptr;
        if (h && SUCCEEDED(GetThreadDescription(h, &wn)) && wn) {
            char buf[128]; WideCharToMultiByte(CP_UTF8, 0, wn, -1, buf, sizeof buf, NULL, NULL); name = buf; LocalFree(wn);
        }
        if (h) CloseHandle(h);
        for (auto& c : name) if (c == ' ') c = '_';
        std::vector<std::pair<unsigned, uintptr_t>> v;
        for (auto& a : t.second) v.push_back({a.second, a.first});
        std::sort(v.rbegin(), v.rend());
        std::fprintf(stderr, "[prof] thread %lu %s: %u samples, %u%% running in the exe\n", (unsigned long)t.first,
                     name.c_str(), total[t.first], 100u * in_exe / total[t.first]);
        for (size_t i = 0; i < v.size() && i < 25; i++) {
            const uintptr_t pc = v[i].second;
            if (pc >= base && pc < base + 0x10000000) std::fprintf(stderr, "[prof] %lu %s exe+0x%llx %u\n", (unsigned long)t.first, name.c_str(), (unsigned long long)(pc - base), v[i].first);
            else std::fprintf(stderr, "[prof] %lu %s abs 0x%llx %u\n", (unsigned long)t.first, name.c_str(), (unsigned long long)pc, v[i].first);
        }
    }
    for (auto& t : th) CloseHandle(t.second);
    return 0;
}
extern "C" void hv_prof_start(void)
{
    const char* e = getenv("HV_PROF");
    if (!e || atoi(e) <= 0) return;
    const char* colon = strchr(e, ':');
    const long delay = colon ? atol(e) : 0, secs = colon ? atol(colon + 1) : atol(e);
    CreateThread(NULL, 0, hv_prof_thread_win, (LPVOID)(intptr_t)((delay << 16) | (secs & 0xFFFF)), 0, NULL);
}
#endif
