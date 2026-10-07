/*
 * wpe-browser: the WPE WebKit browser of Phoenix-RTOS (browser plan, milestones B4-B6).
 *
 * One static multi-call program (plan decision 2). Phoenix has no shared libraries, so the UI
 * process, the WebProcess and the NetworkProcess are the same ELF: WebKit's ProcessLauncher
 * (patch 0007) execs WPE_PHOENIX_EXECUTABLE -- this program, as recorded below -- with the
 * child's role in WPE_PHOENIX_PROCESS_ROLE and WebKit's usual argv (<identifier> <socket>).
 *
 * UI role, a small browser on WPEPlatform:
 *   wpe-browser [options] [URL|FILE|WORDS]
 *     --headless            no window: WPEPlatform's headless display (B4)
 *     --snapshot=FILE.png   after the first load finishes, save a snapshot of the visible
 *                           area as PNG, print its checksum and exit
 *     --size=WxH            view size (default 1024x768; Wayland: the initial window size)
 *     --timeout=S           exit with status 2 if the page has not finished loading after S s
 *     --exit-after-load     exit once the first load has finished (window mode)
 *     --ignore-tls-errors   accept invalid certificates
 *     --cpu-rendering       paint with Skia's CPU raster (WEBKIT_SKIA_ENABLE_CPU_RENDERING=1)
 *     --dmabuf              window mode: the web process hands its frames to the compositor as
 *                           dma-bufs instead of reading them back into shared memory (WebKit
 *                           patch 0016); the default where the compositor offers linux-dmabuf
 *     --shm                 frames through shared memory instead: read back by the web process,
 *                           copied by this process (WPE_BROWSER_DMABUF=0; a headless view always)
 *     --frame-ahead         the web process renders the next frame while the compositor shows
 *                           this one, instead of after its frame callback (WebKit patch 0019;
 *                           WPE_BROWSER_FRAME_AHEAD=1)
 *     --webgl               WebGL on (a build with the port's USE flag webgl; WPE_BROWSER_WEBGL=1)
 *     --present-stats=S     every S s, the frames the view presented and what they were made of
 *                           (WPE_BROWSER_PRESENT_SECS)
 *     --web-extensions=DIR  the WebProcess loads the web process extensions (*.so) in DIR,
 *                           with the user data string "wpe-browser"
 *     --ephemeral           keep nothing on disk (the default with --headless). WebKit prints no
 *                           console messages of an ephemeral session's pages (FrameConsoleClient)
 *     --data-dir=DIR        cookies, local storage, HSTS (default $HOME/.local/share/wpe-browser)
 *     --cache-dir=DIR       the HTTP disk cache (default $HOME/.cache/wpe-browser)
 *     --toolbar=MODE        always (default): the toolbar stays at the top and the page is laid
 *                           out below it; auto: it shows with Ctrl+L or the pointer at the top
 *                           edge; never: no toolbar (WPE_BROWSER_TOOLBAR; headless: never)
 *     --no-chrome           the same as --toolbar=never
 *     --search=PREFIX       where plain words go (default DuckDuckGo's HTML search)
 *     --autoplay=POLICY     <video>/<audio> autoplay in a build with media (USE video): muted
 *                           (default, WebKit's: only without sound), allow, deny
 *                           (WPE_BROWSER_AUTOPLAY)
 *     --mse=MODE            Media Source Extensions in a build with them (USE mse): on (default:
 *                           MediaSource, as hls.js, dash.js and Shaka use it), managed (also
 *                           ManagedMediaSource, which hls.js prefers when present), off (sites fall
 *                           back to native HLS or plain files) (WPE_BROWSER_MSE)
 *     --stock-features      keep WebKit 2.54's defaults for the CSS features this program turns
 *                           on (see enableCSSFeatures(); WPE_BROWSER_STOCK_FEATURES=1)
 *     --memory-limit=MB     the web processes' memory pressure handler (WebKit's periodic monitor)
 *                           measures against MB instead of min(RAM, 3 GB): it releases caches from
 *                           0.33 x MB (conservative) and harder from 0.5 x MB (strict)
 *                           (WPE_BROWSER_MEMORY_LIMIT_MB)
 *     --memory-kill=F       ... and a web process above F x MB is terminated, the view reporting
 *                           "web-process-terminated reason=memory-limit" (F > 0.5; default 0, never;
 *                           WPE_BROWSER_MEMORY_KILL)
 *     --memory-poll-secs=S  how often it measures (default 30; WPE_BROWSER_MEMORY_POLL_SECS)
 *                           The web process logs what the monitor does: "WPEB-MEMPRESSURE ..." (WebKit
 *                           patch 0024)
 *   WPE_BROWSER_LIST_FEATURES=1 logs every WebKit feature once at start-up, one line each:
 *   "features list <identifier> status=<status> default=0|1 enabled=0|1"
 *   The process model (WebKit patch 0015; also from the environment). WebKit's own defaults are a
 *   desktop's: on a 4 GB Pi ~15 cached web processes and a prewarmed spare one.
 *     --process-cache=N     keep the web processes of at most N recently left sites, for a quick
 *                           return (WPE_BROWSER_PROCESS_CACHE; default 2, 0 none)
 *     --prewarm             keep a spare web process launched ahead (WPE_BROWSER_PREWARM=1)
 *     --no-process-swap     one web process for every site (WPE_BROWSER_PROCESS_SWAP=0); the
 *                           default is WebKit's, a process per site, swapped on navigation
 *     --hang-recovery=S     when a navigation's web process stays unresponsive for S s,
 *                           terminate it and load the page again (WPE_BROWSER_HANG_SECS;
 *                           default 30, 0 off)
 *     --stall-secs=S        every process, the UI included, reports a main thread that has not
 *                           run its event loop for S s, or stays in one start-up phase as long,
 *                           and a frame pipeline stuck as long (WPE_BROWSER_STALL_SECS; default
 *                           10, 0 off); see stallReport() and frameStallStep()
 *     --frame-stall-secs=S  the UI also reports a view that presented frames and then none for
 *                           S s: for a page that is expected to animate (WPE_BROWSER_FRAME_STALL_SECS;
 *                           default 0, off); see frameStallStep()
 *   Test knobs (also from the environment, for runs started where only one argument fits):
 *     --cycle=LIST          load the next page of LIST every --cycle-secs (WPE_BROWSER_CYCLE):
 *                           comma-separated entries, or the path of a file with one per line
 *     --cycle-secs=S        default 60 (WPE_BROWSER_CYCLE_SECS)
 *     --rss-secs=S          every process logs its memory footprint every S s (WPE_BROWSER_RSS_SECS);
 *                           a child also logs where it is ("mem-map": the anonymous memory of its
 *                           map entries grouped by entry size, and its thread count; see logMapBreakdown())
 *     --auto=STEPS          synthetic keyboard input (WPE_BROWSER_AUTO): comma-separated
 *                           <seconds>:key:<keys> (e.g. ctrl+l, alt+Left, F5, Return) or
 *                           <seconds>:type:<text>, fed through the same path as real keys
 *
 * The chrome (window mode) is an overlay the program injects into every top-level page, in its
 * own script world "wpe-browser": a toolbar (back, forward, reload/stop, home, the address) and a
 * progress line while a page loads. With --toolbar=always (the default) the toolbar is always
 * shown and a user style sheet lays the page out below it; with --toolbar=auto it appears with
 * Ctrl+L or when the pointer touches the top edge, over the page. The address is edited here, in the UI process; the overlay only displays it, so a page
 * never sees those keystrokes.
 *   Keys: Ctrl+L / Alt+D / F6 the address (Enter go, Escape cancel; plain words are a search),
 *   Alt+Left / Alt+Right back / forward, Ctrl+R / F5 reload, Ctrl+Shift+R reload without the
 *   cache, Escape stop, Alt+Home the start page, F11 fullscreen, Ctrl+Q quit.
 * Every line this program prints starts with "WPEB " (UART-friendly, one event per line);
 * every chrome action prints "WPEB t=<ms> chrome action=<what> source=key|ui|auto|cycle ...".
 * The navigation's way through the processes is logged too: "policy navigation" (the page's
 * current web process asked whether to follow it), "page-swap page-id=" (WebKit moved the view to
 * another web process), "web-process responsive=0|1", "load ..." and "load-failed",
 * "web-process-terminated", "hang-recovery ...".
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cmakeconfig.h"

#include <algorithm>
#include <atomic>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/threads.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <glib-unix.h>
#include <png.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#if ENABLE_WPE_PLATFORM_HEADLESS
#include <wpe/headless/wpe-headless.h>
#endif
#if ENABLE_WPE_PLATFORM_WAYLAND
#include <wpe/wayland/wpe-wayland.h>
#endif

namespace WebKit {
int WebProcessMain(int argc, char** argv);
int NetworkProcessMain(int argc, char** argv);
}

namespace WTF {
/* WTF/wtf/phoenix/MemoryFootprintPhoenix.cpp (patch 0003): the anonymous pages of this
 * process's map entries, from meminfo() */
size_t memoryFootprint();
}

/* glib-networking's OpenSSL backend, linked statically: its module entry point registers the
 * GTlsBackend (a static program has no GIO module loading). */
extern "C" void g_io_openssl_load(GIOModule* module);

static double nowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double startMs;

/* One line, one write(2): no stdio. A child stuck in exit() can hold the stdio locks for good
 * (b32-mem: 12 children past "main returned" never ended, each holding ~330 MB), and a watchdog
 * that logged through stderr then waited behind it instead of ending the process. */
static void logLine(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void logLine(const char* format, ...)
{
    char line[1024];
    int n = snprintf(line, sizeof(line), "WPEB t=%.0f ", nowMs() - startMs);
    va_list args;
    va_start(args, format);
    int m = vsnprintf(line + n, sizeof(line) - n - 1, format, args);
    va_end(args);
    n += std::min(std::max(m, 0), static_cast<int>(sizeof(line)) - n - 2);
    line[n++] = '\n';
    for (int off = 0; off < n;) {
        ssize_t w = write(STDERR_FILENO, line + off, n - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        off += static_cast<int>(w);
    }
}

#define LOG(...) logLine(__VA_ARGS__)

static void logFootprint(const char* role)
{
    LOG("mem role=%s pid=%d footprint_kb=%zu", role, static_cast<int>(getpid()), WTF::memoryFootprint() / 1024);
}

/* --- roles ---------------------------------------------------------------------------------- */

/* The absolute path of this executable, for the children (WebKit execs it by path). */
static void recordExecutablePath(const char* argv0)
{
    if (getenv("WPE_PHOENIX_EXECUTABLE"))
        return;
    char resolved[PATH_MAX];
    if (strchr(argv0, '/')) {
        if (realpath(argv0, resolved))
            setenv("WPE_PHOENIX_EXECUTABLE", resolved, 1);
        return;
    }
    /* a bare name: the first PATH entry holding it */
    const char* path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/bin";
    char* copy = strdup(path);
    for (char* dir = strtok(copy, ":"); dir; dir = strtok(nullptr, ":")) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, argv0);
        if (access(candidate, X_OK) == 0 && realpath(candidate, resolved)) {
            setenv("WPE_PHOENIX_EXECUTABLE", resolved, 1);
            break;
        }
    }
    free(copy);
}

/*
 * The main-thread stall report (every process, the UI included; WPE_BROWSER_STALL_SECS, default
 * 10 s, 0 off).
 *
 * A web process whose main thread stops running its event loop stops answering the UI: no
 * navigation reaches it any more, and WebKit only marks it unresponsive. This says where it
 * stopped. The main loop beats once a second (a GLib timeout on the default main context, which
 * WebKit's main RunLoop runs). When the last beat is older than the limit, the watchdog thread
 * below reports, at once and again every 60 s while the stall lasts:
 *
 *   stall n=K main_ms=M ipc_in=0|1 ipc_revents=0x..
 *       ipc_in: the socket of the UI connection holds unread input. 1 = the UI's messages wait for
 *       a main thread that does not take them; 0 while the UI reports the process unresponsive =
 *       the UI's sends do not arrive (the connection, not this process); -1 in the UI (no such
 *       socket)
 *
 * Before the loop's first beat a process is starting: its main thread runs the start-up phases
 * that startupPhase() names (the UI: "start", "display", "network-session" -- persistent:
 * "session-new", "network-launch", "cookie-settings" --, "web-context", "web-view", "first-load",
 * "main-loop"; a child: "process-main"). A phase that lasts longer than the limit is a start-up
 * stall, the same report under another first line, and its end:
 *
 *   start-stall phase=P phase_ms=M report=R
 *   start-stall-end phase=P phase_ms=M
 *
 * (the b29 soak: the UI blocked for 14 minutes between "web-extensions" and "session persistent"
 * and said nothing, as the loop had never beaten). Both stalls go on with:
 *
 *   stall-thread tid=T [main] state=ready|sleep cpu_ms=C delta_ms=D wait_ms=W prio=P
 *       every thread of the process (the kernel's threadsinfo): D is the CPU time since the
 *       previous report, so a busy thread has D close to the time between reports, a blocked one 0
 *   stall-child pid=C tid=T state=ready|sleep cpu_ms=C wait_ms=W name=N
 *       every thread of a child process (a process launch that never got to the child's main():
 *       WebKit spawns with posix_spawn(), whose vfork() keeps the caller's thread waiting until
 *       the child has exec()ed)
 *   stall-sample tid=T [main] pc=.. lr=.. fp=.. sp=..   (or "none")
 *       the registers where SIGUSR2 interrupted each thread, in reports 2-4 of a stall only (from
 *       60 s on: a long task on a slow page must not get its system calls interrupted);
 *       "none" = no answer in 500 ms (the signal blocked, e.g. a thread JSC holds suspended)
 *   stall-stack tid=T ret=0x.. 0x.. ...
 *       the thread's return addresses from its stack: words in the program's code just after a
 *       BL/BLR (WebKit is built without frame pointers, so no fp chain), the nearest first. The
 *       main thread's up to main()'s frame; another thread's up to 64 KiB above its sp, within the
 *       map entry holding its sp (its stack). Symbolise with
 *       addr2line -f -C -e <port install>/bin/wpe-browser <pc> <lr> <ret...>
 *   stall-end n=K main_ms=M   the loop ran again
 *
 * And when the loop runs but the connection's socket has held unread input at every check (once
 * a second) for 3x the limit, the thread that receives it (WebKit's connection work queue) is
 * stuck, or its poll() missed the wakeup:
 *
 *   ipc-stall readable_ms=R main_beat_ms=B ipc_revents=0x..   then the thread lines as above
 *   ipc-stall-end readable_ms=R
 *
 * The signal and its handler cost nothing until a stall: SIGUSR2 is not used by WebKit (JSC's
 * thread suspension is SIGUSR1), and the handler only copies registers and reads the stack.
 */
static constexpr unsigned orphanPollMs = 100;
static constexpr unsigned orphanGraceMs = 1500;
static constexpr unsigned stallRepeatMs = 60000;
static constexpr unsigned stallFirstSampledReport = 1; /* the report 60 s into a stall */
static constexpr unsigned exitGraceMs = 5000; /* a child's exit() after its main returned */
static constexpr unsigned stallLastSampledReport = 3;
static constexpr unsigned maxSamples = 64;
static constexpr unsigned maxStackReturns = 32;
static constexpr size_t maxStackScan = 512 * 1024;
static constexpr int defaultStallSecs = 10;
static constexpr int ipcStallFactor = 3; /* unread connection input for 3x the stall limit */
static constexpr size_t maxThreadStackScan = 64 * 1024; /* another thread's stack, above its sp */
static constexpr unsigned frameStallSampleMs = 20000; /* a frame stall's sampled report, after its first */
static constexpr unsigned reportRequestMinMs = 10000; /* a peer's report requests, at most this often */
static constexpr unsigned presentStallMinFrames = 10; /* --frame-stall-secs: frames before it watches */

static char processRole[16]; /* "ui", "web" or "network" */
static int mainTid;
static uintptr_t mainStackTop; /* main()'s frame: the main thread's stack is mapped from sp up to here */
static int ipcFd = -1; /* a child's UI connection (argv[2]); the UI has none */
static std::atomic<int64_t> lastBeatMs; /* nowMs() of the main loop's last beat; 0: none yet */
static std::atomic<const char*> startPhase { "start" }; /* until the first beat: what main() runs */
static std::atomic<int64_t> startPhaseMs;
static std::atomic<int64_t> exitStartMs; /* a child: nowMs() when its main returned (exit() runs) */
static std::atomic<int> childExitStatus;
static std::atomic<bool> reportRequested; /* SIGINFO: a peer process asks for this one's report */
/* the UI: the frames the view presented (WPEView::buffer-rendered), for --frame-stall-secs */
static std::atomic<unsigned> presentedTotal;
static std::atomic<int64_t> presentedLastMs;
static std::atomic<int> presentStallMs; /* --frame-stall-secs in ms; 0: off */

/* WebKit patch 0020 (PhoenixFrameWatch.h): this process's frame pipeline as one line, and the
 * kind of stall it is in (0: none) when a wait in it has lasted longer than limitMs. ui selects
 * the UI process's half (the backing store, the display link) or the web process's (the
 * compositor, the rendering updates). */
extern "C" int wpe_phoenix_frame_watch(int ui, int64_t limitMs, char* line, size_t size);

/* the start-up step the main thread enters (a start-up stall report names it) */
static void startupPhase(const char* name)
{
    startPhaseMs.store(static_cast<int64_t>(nowMs()));
    startPhase.store(name);
}

/* the program's code: .init starts it and .fini follows .text (a static ELF, crti/crtn) */
extern "C" void _init(void);
extern "C" void _fini(void);

struct ThreadSample {
    std::atomic<int> tid;
    uint64_t pc, lr, fp, sp;
    unsigned returns;
    uint64_t ret[maxStackReturns];
};
static ThreadSample samples[maxSamples];
static std::atomic<unsigned> sampleSlots;

static bool isReturnAddress(uint64_t value)
{
    const uint64_t codeStart = reinterpret_cast<uintptr_t>(&_init), codeEnd = reinterpret_cast<uintptr_t>(&_fini);
    if (value < codeStart + 4 || value >= codeEnd || (value & 3))
        return false;
    const uint32_t insn = *reinterpret_cast<const uint32_t*>(value - 4);
    return (insn & 0xfc000000U) == 0x94000000U /* BL */ || (insn & 0xfffffc1fU) == 0xd63f0000U /* BLR */;
}

/* the return addresses on a stack between sp and top, the nearest first */
static void scanStack(ThreadSample& s, uintptr_t top)
{
    for (auto* p = reinterpret_cast<const uint64_t*>(s.sp & ~uint64_t(7)); reinterpret_cast<uintptr_t>(p) < top && s.returns < maxStackReturns; ++p) {
        if (isReturnAddress(*p))
            s.ret[s.returns++] = *p;
    }
}

static void sampleHandler(int, siginfo_t*, void* context)
{
    const unsigned slot = sampleSlots.fetch_add(1);
    if (slot >= maxSamples)
        return;
    const auto& mc = static_cast<ucontext_t*>(context)->uc_mcontext;
    ThreadSample& s = samples[slot];
    s.pc = mc.pc;
    s.lr = mc.regs[30];
    s.fp = mc.regs[29];
    s.sp = mc.sp;
    s.returns = 0;
    const int tid = gettid();
    if (tid == mainTid && mainStackTop > s.sp && mainStackTop - s.sp <= maxStackScan)
        scanStack(s, mainStackTop);
    s.tid.store(tid, std::memory_order_release);
}

/* SIGINFO: another process of this browser asks for this one's report (frameStallStep()) */
static void reportRequestHandler(int)
{
    reportRequested.store(true);
}

/* this process's map entries (meminfo()), to bound the stack scan of a thread other than main */
static std::vector<entryinfo_t> mapEntries()
{
    std::vector<entryinfo_t> map(256);
    for (int attempt = 0; attempt < 4; ++attempt) {
        meminfo_t info;
        memset(&info, 0, sizeof(info));
        info.page.mapsz = -1;
        info.maps.mapsz = -1;
        info.entry.kmapsz = -1;
        info.entry.pid = static_cast<unsigned>(getpid());
        info.entry.mapsz = static_cast<int>(map.size());
        info.entry.map = map.data();
        meminfo(&info);
        if (info.entry.mapsz < 0)
            break;
        if (static_cast<size_t>(info.entry.mapsz) <= map.size()) {
            map.resize(static_cast<size_t>(info.entry.mapsz));
            return map;
        }
        map.resize(static_cast<size_t>(info.entry.mapsz) + 64);
    }
    return { };
}

/*
 * "mem-map role=R pid=P threads=T entries=E anon_kb=A groups=SIZEk*N=ANONk,... rest=ANONk": the
 * anonymous pages of this process's map entries (what the footprint line sums) grouped by the
 * entry's size, the ten largest groups first. Repeated sizes are the process's kinds of memory: thread stacks (one entry each, the
 * requested size; WebKit's threads 1 MiB, the media player's own, libphoenix's default 256 KiB),
 * mimalloc's arenas, the JIT pool. With the thread count it tells stacks from heap.
 */
static void logMapBreakdown()
{
    const std::vector<entryinfo_t> map = mapEntries();
    struct Group {
        size_t sizeKB;
        unsigned count;
        size_t anonKB;
    };
    std::vector<Group> groups;
    size_t totalKB = 0;
    for (const auto& e : map) {
        /* as WTF::memoryFootprint(): an entry without anonymous pages is marked ~0U */
        if (e.anonsz == static_cast<size_t>(~0U) || e.anonsz == SIZE_MAX)
            continue;
        const size_t sizeKB = e.size / 1024, anonKB = e.anonsz / 1024;
        totalKB += anonKB;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.sizeKB == sizeKB; });
        if (it == groups.end())
            groups.push_back({ sizeKB, 1, anonKB });
        else {
            it->count++;
            it->anonKB += anonKB;
        }
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.anonKB > b.anonKB; });
    const pid_t pid = getpid();
    int threads = 0;
    std::vector<threadinfo_t> info(static_cast<size_t>(std::max(threadcount(), 64) + 64));
    const int count = std::min(threadsinfo(static_cast<int>(info.size()), PH_THREADINFO_ALL, info.data()), static_cast<int>(info.size()));
    for (int i = 0; i < count; i++) {
        if (info[i].pid == pid)
            threads++;
    }
    char list[640];
    size_t used = 0, restKB = totalKB;
    list[0] = '\0';
    for (size_t i = 0; i < groups.size() && i < 10; i++) {
        const int n = snprintf(list + used, sizeof(list) - used, "%s%zuk*%u=%zuk", used ? "," : "", groups[i].sizeKB, groups[i].count, groups[i].anonKB);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(list) - used)
            break;
        used += static_cast<size_t>(n);
        restKB -= groups[i].anonKB;
    }
    LOG("mem-map role=%s pid=%d threads=%d entries=%zu anon_kb=%zu groups=%s rest=%zuk", processRole, static_cast<int>(pid), threads,
        map.size(), totalKB, used ? list : "-", restKB);
}

static gboolean heartbeat(gpointer)
{
    lastBeatMs.store(static_cast<int64_t>(nowMs()));
    return G_SOURCE_CONTINUE;
}

/* before the role's main (on its main thread): the beat on the main context, the sampling
 * signal; ipc: a child's UI connection, -1 in the UI */
static void initStallReport(int ipc)
{
    mainTid = gettid();
    ipcFd = ipc;
    startupPhase("start");
    /* the beat says the loop runs; until its first one, the start-up phases are watched. At the
     * default priority: a G_PRIORITY_HIGH source ready in an iteration makes GLib skip the
     * check() of WPEPlatform's Wayland event source, which left its prepared read held and the
     * UI's main thread waiting on itself in wl_display_read_events() (b31-gate1; patch 0018
     * makes the source safe against it, and the beat should not outrank real work anyway) */
    g_timeout_add_full(G_PRIORITY_DEFAULT, 1000, heartbeat, nullptr, nullptr);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = sampleHandler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR2, &action, nullptr);
    signal(SIGINFO, reportRequestHandler);
}

struct ThreadCPU {
    unsigned tid;
    long long cpuUs;
};

/* the UI connection's socket holds unread input (-1: no socket); *revents gets poll()'s answer */
static int ipcInput(unsigned* revents)
{
    if (ipcFd < 0)
        return -1;
    struct pollfd ipc = { ipcFd, POLLIN, 0 };
    const int polled = poll(&ipc, 1, 0);
    *revents = polled > 0 ? static_cast<unsigned>(ipc.revents) : 0U;
    return polled > 0 && (ipc.revents & POLLIN) ? 1 : 0;
}

/* after the caller's first line: every thread, and in the first reports their registers */
static void stallReport(unsigned report, std::vector<ThreadCPU>& previous)
{
    const int pid = getpid();

    /* this process's threads, from the kernel's list of all of them */
    std::vector<threadinfo_t> info(static_cast<size_t>(std::max(threadcount(), 64) + 64));
    int count = threadsinfo(static_cast<int>(info.size()), PH_THREADINFO_ALL, info.data());
    count = std::min(count, static_cast<int>(info.size()));
    std::vector<ThreadCPU> current;
    std::vector<int> tids;
    const int self = gettid();
    for (int i = 0; i < count; i++) {
        if (info[i].pid != pid) {
            if (info[i].ppid == pid)
                LOG("role=%s pid=%d stall-child pid=%d tid=%u state=%s cpu_ms=%lld wait_ms=%lld name=%.64s", processRole, pid,
                    static_cast<int>(info[i].pid), info[i].tid, info[i].state ? "sleep" : "ready",
                    static_cast<long long>(info[i].cpuTime) / 1000, static_cast<long long>(info[i].wait) / 1000, info[i].name);
            continue;
        }
        long long previousUs = -1;
        for (const auto& p : previous) {
            if (p.tid == info[i].tid)
                previousUs = p.cpuUs;
        }
        const long long cpuUs = static_cast<long long>(info[i].cpuTime);
        LOG("role=%s pid=%d stall-thread tid=%u%s state=%s cpu_ms=%lld delta_ms=%lld wait_ms=%lld prio=%d", processRole, pid,
            info[i].tid, static_cast<int>(info[i].tid) == mainTid ? " main" : static_cast<int>(info[i].tid) == self ? " watchdog" : "",
            info[i].state ? "sleep" : "ready", cpuUs / 1000, previousUs < 0 ? -1LL : (cpuUs - previousUs) / 1000,
            static_cast<long long>(info[i].wait) / 1000, info[i].priority);
        current.push_back({ info[i].tid, cpuUs });
        if (static_cast<int>(info[i].tid) != self)
            tids.push_back(static_cast<int>(info[i].tid));
    }
    previous = std::move(current);
    if (report < stallFirstSampledReport || report > stallLastSampledReport)
        return;

    /* where each thread is: SIGUSR2, then what the handlers left */
    for (auto& s : samples)
        s.tid.store(0);
    sampleSlots.store(0);
    unsigned sent = 0;
    for (int tid : tids) {
        if (!sys_tkill(pid, tid, SIGUSR2))
            sent++;
    }
    for (unsigned waited = 0; waited < 500 && std::min(sampleSlots.load(), maxSamples) < sent; waited += 10)
        usleep(10 * 1000);
    usleep(10 * 1000); /* the last handler's stores */
    const auto map = mapEntries();
    for (int tid : tids) {
        ThreadSample* found = nullptr;
        for (auto& s : samples) {
            if (s.tid.load(std::memory_order_acquire) == tid)
                found = &s;
        }
        if (found && tid != mainTid) {
            /* its stack is the map entry holding its sp: scanned from here, the thread has
             * answered the signal and is back where it was */
            for (const auto& entry : map) {
                const uintptr_t start = reinterpret_cast<uintptr_t>(entry.vaddr), end = start + entry.size;
                if (found->sp >= start && found->sp < end) {
                    scanStack(*found, std::min<uintptr_t>(end, found->sp + maxThreadStackScan));
                    break;
                }
            }
        }
        const char* mark = tid == mainTid ? " main" : "";
        if (!found) {
            LOG("role=%s pid=%d stall-sample tid=%d%s none", processRole, pid, tid, mark);
            continue;
        }
        LOG("role=%s pid=%d stall-sample tid=%d%s pc=0x%llx lr=0x%llx fp=0x%llx sp=0x%llx", processRole, pid, tid, mark,
            static_cast<unsigned long long>(found->pc), static_cast<unsigned long long>(found->lr),
            static_cast<unsigned long long>(found->fp), static_cast<unsigned long long>(found->sp));
        if (found->returns) {
            std::string line;
            char word[24];
            for (unsigned i = 0; i < found->returns; i++) {
                snprintf(word, sizeof(word), " 0x%llx", static_cast<unsigned long long>(found->ret[i]));
                line += word;
            }
            LOG("role=%s pid=%d stall-stack tid=%d ret=%s", processRole, pid, tid, line.c_str() + 1);
        }
    }
}

/*
 * The frame watch (WebKit patch 0020; every web process and the UI, once a second, with the stall
 * limit of --stall-secs). A page can stop drawing while every main loop runs: the compositing
 * thread stuck inside a frame, a rendering update waiting for tiles or for the UI's frame-done,
 * the UI waiting for the compositor's frame callback. The patch records where each frame is; a
 * wait older than the limit is a frame stall:
 *
 *   frame-stall n=K report=R frame-watch kind=KIND compositor state=.. phase=.. ...   (web)
 *   frame-stall n=K report=R frame-watch kind=KIND backing-store received=.. ...     (UI)
 *       KIND, web: render (the compositing thread inside one step of a frame: phase=), tiles
 *       (a rendering update waits for tiles still painting), frame-done (the UI never answered
 *       the frame), no-frame (a composition sent no frame, so none can complete), scheduled (a
 *       composition due that the compositing thread never ran), renderer (the main thread
 *       waits for a composition that never reports back), no-refresh (requestAnimationFrame
 *       asked the UI's display link for a refresh and none came: patch 0022); UI: ui-pending (a
 *       received frame not handed to the view), ui-callback (the compositor never sent the
 *       frame callback)
 *
 * Report 0 comes with the thread list (stall-thread), report 1, 20 s later if the stall lasts,
 * with every thread's registers and stack (stall-sample, stall-stack), and then asks the peer
 * processes for theirs (SIGINFO: the UI asks its children, a web process its UI), which answer
 *
 *   report-request frame-watch kind=.. ...   then their stall-thread/-sample/-stack lines
 *
 * and frame-stall-end n=K ms=M when the frames move again. Nothing is stuck when a page stops
 * because no requestAnimationFrame callback runs any more (a script that threw, a display link
 * that stopped): for a page that is expected to animate, --frame-stall-secs=S makes the UI watch
 * the frames its view presents as well, with the same two reports:
 *
 *   present-stall n=K report=R presented=N idle_ms=M frame-watch kind=.. backing-store ...
 *
 * The UI's line also counts the display link's ticks and the ticks it sent (fired=) to a web
 * process's requestAnimationFrame. A present stall asks the peers for their reports with its first
 * report already (a frame stall only with its second, 20 s on).
 */
struct FrameStall {
    explicit FrameStall(const char* name)
        : what(name)
    {
    }
    const char* what; /* "frame-stall" or "present-stall" */
    bool peersAtFirstReport = false; /* ask the peers for theirs with report 0, not only report 1 */
    unsigned count = 0;
    double since = 0; /* when the current one began; 0: none */
    unsigned reports = 0;
    std::vector<ThreadCPU> cpu;
};

/* the UI's children (the web and network processes) or a child's UI: report too */
static void askPeersForReports()
{
    const pid_t self = getpid();
    if (strcmp(processRole, "ui")) {
        kill(getppid(), SIGINFO);
        return;
    }
    std::vector<threadinfo_t> info(static_cast<size_t>(std::max(threadcount(), 64) + 64));
    int count = threadsinfo(static_cast<int>(info.size()), PH_THREADINFO_ALL, info.data());
    count = std::min(count, static_cast<int>(info.size()));
    std::vector<pid_t> children;
    for (int i = 0; i < count; i++) {
        const pid_t pid = static_cast<pid_t>(info[i].pid);
        if (static_cast<pid_t>(info[i].ppid) == self && std::find(children.begin(), children.end(), pid) == children.end())
            children.push_back(pid);
    }
    for (pid_t child : children)
        kill(child, SIGINFO);
}

/* one detector's step: stalled or not now, and the line describing the pipeline */
static void frameStallStep(FrameStall& stall, bool stalled, const char* line)
{
    const double now = nowMs();
    const int pid = static_cast<int>(getpid());
    if (!stalled) {
        if (stall.since)
            LOG("role=%s pid=%d %s-end n=%u ms=%.0f", processRole, pid, stall.what, stall.count, now - stall.since);
        stall.since = 0;
        return;
    }
    if (!stall.since) {
        stall.since = now;
        stall.count++;
        stall.reports = 0;
        stall.cpu.clear();
    }
    if (stall.reports > 1 || now < stall.since + stall.reports * frameStallSampleMs)
        return;
    LOG("role=%s pid=%d %s n=%u report=%u %s", processRole, pid, stall.what, stall.count, stall.reports, line);
    stallReport(stall.reports ? stallFirstSampledReport : 0, stall.cpu);
    if (stall.reports++ || stall.peersAtFirstReport)
        askPeersForReports();
}

/*
 * A child never outlives the UI process. WebKit's children exit when they see their IPC
 * connection to the UI close, and back that up with 10 s watchdogs (the WebProcess's ends in
 * g_error(), which on Phoenix raises SIGTRAP rather than calling abort(), as GLib finds no
 * /proc/self/status). This is the Phoenix counterpart of Linux's PR_SET_PDEATHSIG: once the UI
 * is gone (the child has been reparented) the child gets a short grace period for WebKit's own
 * orderly exit, then _exit()s - so the next browser run never meets the previous run's children.
 * The same thread logs the child's memory footprint every WPE_BROWSER_RSS_SECS seconds and makes
 * the stall report above. In the UI process (parent 0: nothing to outlive; its footprint has a
 * main-loop timer) the thread makes the stall report only.
 */
static void* watchdog(void* arg)
{
    const pid_t parent = static_cast<pid_t>(reinterpret_cast<intptr_t>(arg));
    const char* rss = parent ? getenv("WPE_BROWSER_RSS_SECS") : nullptr;
    const unsigned rssPolls = rss ? static_cast<unsigned>(atoi(rss)) * (1000 / orphanPollMs) : 0;
    const char* stall = getenv("WPE_BROWSER_STALL_SECS");
    const double stallMs = (stall ? atoi(stall) : defaultStallSecs) * 1000.0;
    unsigned polls = 0, stalls = 0, reports = 0;
    double stallSince = 0, nextReport = 0, ipcSince = 0;
    bool ipcReported = false;
    const char* stalledPhase = nullptr; /* the start-up phase reported as stalled */
    double stalledPhaseSince = 0;
    std::vector<ThreadCPU> cpu;
    /* the frame watch: the web process's compositor, the UI's backing store and its view */
    const int frameRole = !strcmp(processRole, "ui") ? 1 : !strcmp(processRole, "web") ? 0 : -1;
    FrameStall frameStall("frame-stall"), presentStall("present-stall");
    /* a view that stopped presenting: the web process's frame watch and thread samples come with
     * the first report, while a short pause (B7's ~4 s requestAnimationFrame pauses) still lasts */
    presentStall.peersAtFirstReport = true;
    double lastRequestReport = -static_cast<double>(reportRequestMinMs);
    char frameLine[768];
    while (!parent || getppid() == parent) {
        usleep(orphanPollMs * 1000);
        ++polls;
        if (exitStartMs.load() && nowMs() - static_cast<double>(exitStartMs.load()) > exitGraceMs)
            break; /* exit() hangs: reported and ended below */
        if (rssPolls && polls % rssPolls == 0) {
            logFootprint(processRole);
            logMapBreakdown();
        }
        if (reportRequested.exchange(false) && nowMs() - lastRequestReport >= reportRequestMinMs) {
            /* a peer's frame stall: where this process is */
            lastRequestReport = nowMs();
            frameLine[0] = '\0';
            if (frameRole >= 0)
                wpe_phoenix_frame_watch(frameRole, 0, frameLine, sizeof(frameLine));
            LOG("role=%s pid=%d report-request %s", processRole, static_cast<int>(getpid()), frameLine);
            std::vector<ThreadCPU> none;
            stallReport(stallFirstSampledReport, none);
        }
        if (stallMs > 0 && frameRole >= 0 && polls % (1000 / orphanPollMs) == 0) {
            const int kind = wpe_phoenix_frame_watch(frameRole, static_cast<int64_t>(stallMs), frameLine, sizeof(frameLine));
            frameStallStep(frameStall, kind, frameLine);
            const int presentLimit = presentStallMs.load();
            if (frameRole == 1 && presentLimit > 0) {
                /* --frame-stall-secs: the view had frames, and has had none for the limit */
                const unsigned presented = presentedTotal.load();
                const double idle = nowMs() - static_cast<double>(presentedLastMs.load());
                char line[sizeof(frameLine) + 64];
                snprintf(line, sizeof(line), "presented=%u idle_ms=%.0f %s", presented, idle, frameLine);
                frameStallStep(presentStall, presented >= presentStallMinFrames && idle > presentLimit, line);
            }
        }
        if (stallMs <= 0)
            continue;
        const double now = nowMs(), beat = static_cast<double>(lastBeatMs.load());
        const char* phase = beat ? nullptr : startPhase.load();
        if (stalledPhase && phase != stalledPhase) {
            LOG("role=%s pid=%d start-stall-end phase=%s phase_ms=%.0f", processRole, static_cast<int>(getpid()), stalledPhase,
                now - stalledPhaseSince);
            stalledPhase = nullptr;
            cpu.clear();
        }
        if (!beat) {
            /* starting: the main thread has been in one phase for longer than the limit */
            const double since = static_cast<double>(startPhaseMs.load());
            if (!stalledPhase && now - since > stallMs) {
                stalledPhase = phase;
                stalledPhaseSince = since;
                reports = 0;
                nextReport = now;
            }
            if (stalledPhase && now >= nextReport) {
                LOG("role=%s pid=%d start-stall phase=%s phase_ms=%.0f report=%u", processRole, static_cast<int>(getpid()),
                    stalledPhase, now - stalledPhaseSince, reports);
                stallReport(reports++, cpu);
                nextReport = now + stallRepeatMs;
            }
            continue;
        }
        if (!stallSince && now - beat > stallMs) {
            stallSince = beat;
            stalls++;
            reports = 0;
            nextReport = now;
        }
        if (stallSince && beat > stallSince) {
            LOG("role=%s pid=%d stall-end n=%u main_ms=%.0f", processRole, static_cast<int>(getpid()), stalls, beat - stallSince);
            stallSince = 0;
            cpu.clear();
        }
        if (stallSince && now >= nextReport) {
            unsigned revents;
            const int input = ipcInput(&revents);
            LOG("role=%s pid=%d stall n=%u main_ms=%.0f report=%u ipc_in=%d ipc_revents=0x%x", processRole, static_cast<int>(getpid()),
                stalls, now - stallSince, reports, input, revents);
            stallReport(reports++, cpu);
            nextReport = now + stallRepeatMs;
        }
        /* the main loop runs, but the connection's input stays unread: its receiving thread */
        if (stallSince || polls % (1000 / orphanPollMs))
            continue;
        unsigned revents;
        if (ipcInput(&revents) == 1) {
            if (!ipcSince)
                ipcSince = now;
            else if (!ipcReported && now - ipcSince > ipcStallFactor * stallMs) {
                ipcReported = true;
                LOG("role=%s pid=%d ipc-stall readable_ms=%.0f main_beat_ms=%.0f ipc_revents=0x%x", processRole, static_cast<int>(getpid()),
                    now - ipcSince, now - beat, revents);
                std::vector<ThreadCPU> none;
                stallReport(stallFirstSampledReport, none); /* 30 s of unread input already */
            }
        } else {
            if (ipcReported)
                LOG("role=%s pid=%d ipc-stall-end readable_ms=%.0f", processRole, static_cast<int>(getpid()), now - ipcSince);
            ipcSince = 0;
            ipcReported = false;
        }
    }
    if (const int64_t exitMs = exitStartMs.load()) {
        /* main returned, but exit() (C++ static destructors, atexit handlers, the stdio flush)
         * has not ended the process: where it waits, then the end exit() did not reach */
        if (nowMs() - static_cast<double>(exitMs) < exitGraceMs)
            usleep(static_cast<useconds_t>((exitGraceMs - (nowMs() - static_cast<double>(exitMs))) * 1000));
        LOG("role=%s pid=%d exit-stall exit_ms=%.0f parent=%s", processRole, static_cast<int>(getpid()),
            nowMs() - static_cast<double>(exitMs), getppid() == parent ? "alive" : "gone");
        std::vector<ThreadCPU> none;
        stallReport(stallFirstSampledReport, none);
        LOG("role=%s pid=%d exit-stall _exit(%d)", processRole, static_cast<int>(getpid()), childExitStatus.load());
        _exit(childExitStatus.load());
    }
    usleep(orphanGraceMs * 1000);
    LOG("role=%s pid=%d orphaned (UI pid %d gone %u ms ago), exiting", processRole, static_cast<int>(getpid()),
        static_cast<int>(parent), orphanGraceMs);
    _exit(0);
    return nullptr;
}

/* parent: the UI process of a child, 0 in the UI */
static void startWatchdog(pid_t parent)
{
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if (pthread_create(&thread, &attr, watchdog, reinterpret_cast<void*>(static_cast<intptr_t>(parent))))
        LOG("role=%s pid=%d: no watchdog (pthread_create failed)", processRole, static_cast<int>(getpid()));
    pthread_attr_destroy(&attr);
}

/* --- UI role: options ----------------------------------------------------------------------- */

static constexpr int defaultProcessCache = 2;
static constexpr int defaultHangSecs = 30;

static gboolean optHeadless;
static char* optSnapshot;
static char* optSize;
static int optTimeout;
static gboolean optExitAfterLoad;
static gboolean optIgnoreTLSErrors;
static gboolean optCPURendering;
static gboolean optDMABuf;
static gboolean optSHM;
static bool dmabufAsked; /* --dmabuf or WPE_BROWSER_DMABUF=1: say why when it cannot be used */
static gboolean optFrameAhead;
static gboolean optWebGL;
static int optPresentSecs;
static char* optWebExtensions;
static gboolean optEphemeral;
static char* optDataDir;
static char* optCacheDir;
static gboolean optNoChrome;
static char* optToolbar;
static char* optSearch;
static char* optCycle;
static int optCycleSecs;
static int optRSSSecs;
static char* optAuto;
static int optProcessCache = -1;
static gboolean optPrewarm;
static gboolean optNoProcessSwap;
static int optHangSecs = -1;
static int optStallSecs = -1;
static int optFrameStallSecs = -1;
static gboolean optStockFeatures;
static int optMemoryLimitMB;
static double optMemoryKill;
static double optMemoryPollSecs;
#if ENABLE_VIDEO
static char* optAutoplay;
#endif
#if ENABLE_MEDIA_SOURCE
static char* optMSE;
#endif
static char** optURIs;

static const GOptionEntry optionEntries[] = {
    { "headless", 0, 0, G_OPTION_ARG_NONE, &optHeadless, "No window (WPEPlatform headless display)", nullptr },
    { "snapshot", 0, 0, G_OPTION_ARG_FILENAME, &optSnapshot, "Save a PNG snapshot after the first load and exit", "FILE" },
    { "size", 0, 0, G_OPTION_ARG_STRING, &optSize, "View size", "WxH" },
    { "timeout", 0, 0, G_OPTION_ARG_INT, &optTimeout, "Fail (exit 2) if the first load takes longer", "S" },
    { "exit-after-load", 0, 0, G_OPTION_ARG_NONE, &optExitAfterLoad, "Exit when the first load has finished", nullptr },
    { "ignore-tls-errors", 0, 0, G_OPTION_ARG_NONE, &optIgnoreTLSErrors, "Accept invalid TLS certificates", nullptr },
    { "cpu-rendering", 0, 0, G_OPTION_ARG_NONE, &optCPURendering, "Skia CPU raster in the WebProcess", nullptr },
    { "dmabuf", 0, 0, G_OPTION_ARG_NONE, &optDMABuf, "Frames to the compositor as dma-bufs, no readback (window mode; the default)", nullptr },
    { "shm", 0, 0, G_OPTION_ARG_NONE, &optSHM, "Frames to the compositor through shared memory (read back and copied)", nullptr },
    { "frame-ahead", 0, 0, G_OPTION_ARG_NONE, &optFrameAhead, "Render the next frame while the compositor shows this one", nullptr },
    { "webgl", 0, 0, G_OPTION_ARG_NONE, &optWebGL, "Enable WebGL (a webgl build)", nullptr },
    { "present-stats", 0, 0, G_OPTION_ARG_INT, &optPresentSecs, "Log the frames the view presented every S s", "S" },
    { "web-extensions", 0, 0, G_OPTION_ARG_FILENAME, &optWebExtensions, "Web process extensions directory", "DIR" },
    { "ephemeral", 0, 0, G_OPTION_ARG_NONE, &optEphemeral, "Keep no cookies, cache or site data on disk", nullptr },
    { "data-dir", 0, 0, G_OPTION_ARG_FILENAME, &optDataDir, "Cookies and site data (default $HOME/.local/share/wpe-browser)", "DIR" },
    { "cache-dir", 0, 0, G_OPTION_ARG_FILENAME, &optCacheDir, "HTTP disk cache (default $HOME/.cache/wpe-browser)", "DIR" },
    { "toolbar", 0, 0, G_OPTION_ARG_STRING, &optToolbar, "always (default): pinned at the top; auto: on demand; never", "MODE" },
    { "no-chrome", 0, 0, G_OPTION_ARG_NONE, &optNoChrome, "No toolbar (--toolbar=never)", nullptr },
    { "search", 0, 0, G_OPTION_ARG_STRING, &optSearch, "Search URL prefix for plain words", "PREFIX" },
    { "cycle", 0, 0, G_OPTION_ARG_STRING, &optCycle, "Pages to cycle through (comma list or file)", "LIST" },
    { "cycle-secs", 0, 0, G_OPTION_ARG_INT, &optCycleSecs, "Seconds per cycled page (default 60)", "S" },
    { "rss-secs", 0, 0, G_OPTION_ARG_INT, &optRSSSecs, "Log every process's memory footprint every S s", "S" },
    { "auto", 0, 0, G_OPTION_ARG_STRING, &optAuto, "Synthetic key input: <s>:key:<keys>,<s>:type:<text>,...", "STEPS" },
    { "process-cache", 0, 0, G_OPTION_ARG_INT, &optProcessCache, "Web processes of recently left sites kept (default 2)", "N" },
    { "prewarm", 0, 0, G_OPTION_ARG_NONE, &optPrewarm, "Keep a spare web process launched ahead", nullptr },
    { "no-process-swap", 0, 0, G_OPTION_ARG_NONE, &optNoProcessSwap, "One web process for every site", nullptr },
    { "hang-recovery", 0, 0, G_OPTION_ARG_INT, &optHangSecs, "Restart a web process unresponsive for S s during a navigation (default 30, 0 off)", "S" },
    { "stall-secs", 0, 0, G_OPTION_ARG_INT, &optStallSecs, "Every process reports a main loop (or start-up) stalled for S s (default 10, 0 off)", "S" },
    { "frame-stall-secs", 0, 0, G_OPTION_ARG_INT, &optFrameStallSecs, "Report a view that presented frames, then none for S s (default 0, off)", "S" },
    { "stock-features", 0, 0, G_OPTION_ARG_NONE, &optStockFeatures, "Keep WebKit's defaults for the CSS features this program enables", nullptr },
    { "memory-limit", 0, 0, G_OPTION_ARG_INT, &optMemoryLimitMB, "The web processes' memory pressure handler measures against MB (default min(RAM, 3 GB))", "MB" },
    { "memory-kill", 0, 0, G_OPTION_ARG_DOUBLE, &optMemoryKill, "Terminate a web process above F x the memory limit (F > 0.5; default 0, never)", "F" },
    { "memory-poll-secs", 0, 0, G_OPTION_ARG_DOUBLE, &optMemoryPollSecs, "How often the memory pressure handler measures (default 30)", "S" },
#if ENABLE_VIDEO
    { "autoplay", 0, 0, G_OPTION_ARG_STRING, &optAutoplay, "Media autoplay: muted (default), allow, deny", "POLICY" },
#endif
#if ENABLE_MEDIA_SOURCE
    { "mse", 0, 0, G_OPTION_ARG_STRING, &optMSE, "Media Source Extensions: on (default), managed (also ManagedMediaSource), off", "MODE" },
#endif
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &optURIs, nullptr, "[URL|FILE|WORDS]" },
    { }
};

/* the test knobs come from the environment when not given (XFCE_AUTOSTART passes one argument) */
static void optionsFromEnvironment()
{
    if (!optCycle && g_getenv("WPE_BROWSER_CYCLE"))
        optCycle = g_strdup(g_getenv("WPE_BROWSER_CYCLE"));
    if (!optCycleSecs && g_getenv("WPE_BROWSER_CYCLE_SECS"))
        optCycleSecs = atoi(g_getenv("WPE_BROWSER_CYCLE_SECS"));
    if (!optRSSSecs && g_getenv("WPE_BROWSER_RSS_SECS"))
        optRSSSecs = atoi(g_getenv("WPE_BROWSER_RSS_SECS"));
    if (!optAuto && g_getenv("WPE_BROWSER_AUTO"))
        optAuto = g_strdup(g_getenv("WPE_BROWSER_AUTO"));
    if (optCycleSecs <= 0)
        optCycleSecs = 60;
    if (!optToolbar && g_getenv("WPE_BROWSER_TOOLBAR"))
        optToolbar = g_strdup(g_getenv("WPE_BROWSER_TOOLBAR"));
#if ENABLE_VIDEO
    if (!optAutoplay && g_getenv("WPE_BROWSER_AUTOPLAY"))
        optAutoplay = g_strdup(g_getenv("WPE_BROWSER_AUTOPLAY"));
#endif
#if ENABLE_MEDIA_SOURCE
    if (!optMSE && g_getenv("WPE_BROWSER_MSE"))
        optMSE = g_strdup(g_getenv("WPE_BROWSER_MSE"));
#endif
    auto number = [](const char* name, int fallback) {
        const char* value = g_getenv(name);
        return value && *value ? atoi(value) : fallback;
    };
    /* dma-bufs unless asked otherwise: GPU raster into shared memory reads every frame back and
     * the compositor uploads it again, which held a 1080p30 video at 19 painted frames/s */
    dmabufAsked = optDMABuf || number("WPE_BROWSER_DMABUF", 0) != 0;
    optDMABuf = !optSHM && number("WPE_BROWSER_DMABUF", 1) != 0;
    if (!optFrameAhead)
        optFrameAhead = number("WPE_BROWSER_FRAME_AHEAD", 0) != 0;
    if (!optWebGL)
        optWebGL = number("WPE_BROWSER_WEBGL", 0) != 0;
    if (optPresentSecs <= 0)
        optPresentSecs = std::max(number("WPE_BROWSER_PRESENT_SECS", 0), 0);
    if (optProcessCache < 0)
        optProcessCache = std::max(number("WPE_BROWSER_PROCESS_CACHE", defaultProcessCache), 0);
    if (!optPrewarm)
        optPrewarm = number("WPE_BROWSER_PREWARM", 0) != 0;
    if (!optNoProcessSwap)
        optNoProcessSwap = !number("WPE_BROWSER_PROCESS_SWAP", 1);
    if (optHangSecs < 0)
        optHangSecs = std::max(number("WPE_BROWSER_HANG_SECS", defaultHangSecs), 0);
    if (optStallSecs < 0)
        optStallSecs = std::max(number("WPE_BROWSER_STALL_SECS", defaultStallSecs), 0);
    if (optFrameStallSecs < 0)
        optFrameStallSecs = std::max(number("WPE_BROWSER_FRAME_STALL_SECS", 0), 0);
    if (!optStockFeatures)
        optStockFeatures = number("WPE_BROWSER_STOCK_FEATURES", 0) != 0;
    if (optMemoryLimitMB <= 0)
        optMemoryLimitMB = std::max(number("WPE_BROWSER_MEMORY_LIMIT_MB", 0), 0);
    auto real = [](const char* name) {
        const char* value = g_getenv(name);
        return value && *value ? g_ascii_strtod(value, nullptr) : 0.0;
    };
    if (optMemoryKill <= 0)
        optMemoryKill = std::max(real("WPE_BROWSER_MEMORY_KILL"), 0.0);
    if (optMemoryPollSecs <= 0)
        optMemoryPollSecs = std::max(real("WPE_BROWSER_MEMORY_POLL_SECS"), 0.0);
}

/*
 * CSS features WebKit 2.54 implements but ships off ("testable"), which later WebKit marks stable
 * and turns on by default; each is self-contained and only reached by pages that use it
 * (coordination repo docs/browser/CSS3TEST-GAPS.md). Not the CSS Painting API, which WebKit still
 * keeps behind its experimental features. A feature this WebKit does not list is logged as
 * absent, so an upgrade that renames or drops one shows in the log rather than silently.
 * The identifiers are the API's, not the preference keys: WebKitFeature.cpp drops the keys'
 * "Enabled" suffix (CSSCornerShapeEnabled in UnifiedWebPreferences.yaml is "CSSCornerShape").
 */
static void listFeatures(WebKitSettings* settings, WebKitFeatureList* features)
{
    static const char* const statuses[] = { "embedder", "unstable", "internal", "developer", "testable",
        "preview", "stable", "mature" };
    gsize count = webkit_feature_list_get_length(features);
    for (gsize i = 0; i < count; i++) {
        WebKitFeature* feature = webkit_feature_list_get(features, i);
        unsigned status = webkit_feature_get_status(feature);
        LOG("features list %s status=%s default=%d enabled=%d", webkit_feature_get_identifier(feature),
            status < G_N_ELEMENTS(statuses) ? statuses[status] : "?", webkit_feature_get_default_value(feature) ? 1 : 0,
            webkit_settings_get_feature_enabled(settings, feature) ? 1 : 0);
    }
    LOG("features listed=%zu", static_cast<size_t>(count));
}

static void enableCSSFeatures(WebKitSettings* settings)
{
    static const char* const identifiers[] = {
        "CSSCornerShape",    /* corner-shape, corner-*-shape (Borders 4) */
        "CSSObjectViewBox",  /* object-view-box (Images 5) */
        "CSSIdentFunction",  /* ident() (Values 5) */
    };
    WebKitFeatureList* features = webkit_settings_get_all_features();
    const char* list = g_getenv("WPE_BROWSER_LIST_FEATURES");
    if (optStockFeatures) {
        LOG("features stock");
        if (list && !strcmp(list, "1"))
            listFeatures(settings, features);
        webkit_feature_list_unref(features);
        return;
    }
    std::string enabled, absent;
    for (const char* identifier : identifiers) {
        WebKitFeature* feature = webkit_feature_list_find(features, identifier);
        std::string& to = feature ? enabled : absent;
        to += to.empty() ? identifier : std::string(",") + identifier;
        if (feature)
            webkit_settings_set_feature_enabled(settings, feature, TRUE);
    }
    LOG("features enabled=%s absent=%s", enabled.empty() ? "-" : enabled.c_str(), absent.empty() ? "-" : absent.c_str());
    if (list && !strcmp(list, "1"))
        listFeatures(settings, features);
    webkit_feature_list_unref(features);
}

/*
 * The process model (WebKit patch 0015 reads the WPE_PHOENIX_* variables when the web context is
 * created, so this runs before anything creates it). WebKit's defaults are a desktop's; the Pi's:
 *   - a process per site, swapped on cross-site navigation: WebKit's model, kept. One process
 *     for everything (--no-process-swap) saves memory but gives up the isolation between sites,
 *     and a page that wedges or crashes its process takes every later site with it;
 *   - the WebProcess cache: 2 instead of ~15. A cached process saves a return to a recent site
 *     the ~1.5 s of a process launch (B6 soak: GitHub `load started` 1.4 s after the request in
 *     a new process, 0.3 s in a cached one); two cover going back and forth, and each one more
 *     is a whole idle WebProcess (JSC heap, fonts, GL context) for 5 minutes;
 *   - no prewarmed spare: the B6 soak left four prewarmed processes that never got a page.
 */
static void applyProcessModel()
{
    g_autofree char* cache = g_strdup_printf("%d", optProcessCache);
    g_autofree char* stall = g_strdup_printf("%d", optStallSecs);
    g_setenv("WPE_PHOENIX_PROCESS_CACHE", cache, TRUE);
    g_setenv("WPE_PHOENIX_PREWARM", optPrewarm ? "1" : "0", TRUE);
    g_setenv("WPE_PHOENIX_PROCESS_SWAP", optNoProcessSwap ? "0" : "1", TRUE);
    g_setenv("WPE_BROWSER_STALL_SECS", stall, TRUE); /* for the children's watchdog */
    LOG("process-model process-swap=%d prewarm=%d process-cache=%d hang-recovery=%d stall-secs=%d", !optNoProcessSwap,
        optPrewarm ? 1 : 0, optProcessCache, optHangSecs, optStallSecs);
    if (const char* channels = g_getenv("WEBKIT_DEBUG")) {
#if ENABLE_RELEASE_LOG
        LOG("webkit-debug channels=%s", channels);
#else
        LOG("webkit-debug channels=%s ignored: this build has no release logging (port USE flag release_log)", channels);
#endif
    }
}

/* --- UI role: state ------------------------------------------------------------------------- */

static constexpr const char* startPagePath = "/usr/share/wpe-browser/start.html";
static constexpr const char* defaultSearch = "https://html.duckduckgo.com/html/?q=";
static constexpr const char* chromeWorld = "wpe-browser";

static GMainLoop* mainLoop;
static int exitStatus;
static gboolean firstLoadDone;
static char* homeURI;
static WebKitWebView* webView;

static void quit(int status)
{
    exitStatus = status;
    g_main_loop_quit(mainLoop);
}

/* --- the navigation in flight, and the hang recovery ---------------------------------------- */

/*
 * A navigation this program issues (an address, the cycle, home, back, forward, reload) is
 * pending until it commits or fails. WebKit hands it to the page's current web process first,
 * which asks for the policy decision ("policy navigation") before the load starts, in that
 * process or, after a process swap, in another one. A web process whose main thread no longer
 * runs never answers: WebKit marks it unresponsive (after 3 s) and nothing else happens, so the
 * view would wait forever - the B6 soak's stall. With --hang-recovery=S, a navigation that has
 * waited S s on an unresponsive process gets that process terminated and is issued again, once;
 * a new web process serves it.
 */
struct PendingNavigation {
    char* uri { nullptr }; /* what is loaded again; null: nothing pending */
    const char* kind { "load" };
    double since { 0 };
    bool asked { false }; /* the policy question for it came */
    unsigned questions { 0 }; /* policy questions logged while it is pending */
    bool started { false };
    bool reported { false };
    bool retry { false }; /* this is the recovery's second attempt */
    guint timer { 0 };
};
static PendingNavigation pending;

static guint64 pageID()
{
    return webView ? webkit_web_view_get_page_id(webView) : 0;
}

static void pendingDone()
{
    g_clear_pointer(&pending.uri, g_free);
}

static gboolean retryNavigation(gpointer data)
{
    const char* uri = static_cast<const char*>(data);
    LOG("hang-recovery load uri=%s", uri);
    webkit_web_view_load_uri(webView, uri);
    return G_SOURCE_REMOVE;
}

static void navigationIssued(const char* kind, const char* uri, bool retry = false);

static gboolean pendingCheck(gpointer)
{
    if (!pending.uri) {
        pending.timer = 0;
        return G_SOURCE_REMOVE;
    }
    const double waited = nowMs() - pending.since;
    if (!optHangSecs || waited < optHangSecs * 1000.0)
        return G_SOURCE_CONTINUE;
    const bool responsive = webkit_web_view_get_is_web_process_responsive(webView);
    if (responsive) {
        /* a slow site, a long page or a stuck provisional process: noted once, never killed */
        if (!pending.reported)
            LOG("navigation-wait kind=%s waited_ms=%.0f asked=%d started=%d responsive=1 page-id=%llu uri=%s", pending.kind,
                waited, pending.asked, pending.started, static_cast<unsigned long long>(pageID()), pending.uri);
        pending.reported = true;
        return G_SOURCE_CONTINUE;
    }
    if (pending.retry) {
        LOG("hang-recovery gave-up waited_ms=%.0f page-id=%llu uri=%s", waited, static_cast<unsigned long long>(pageID()), pending.uri);
        pendingDone();
        pending.timer = 0;
        return G_SOURCE_REMOVE;
    }
    LOG("hang-recovery terminate-web-process kind=%s waited_ms=%.0f asked=%d started=%d page-id=%llu uri=%s", pending.kind, waited,
        pending.asked, pending.started, static_cast<unsigned long long>(pageID()), pending.uri);
    char* uri = g_strdup(pending.uri);
    webkit_web_view_terminate_web_process(webView);
    navigationIssued("retry", uri, true);
    g_idle_add_full(G_PRIORITY_DEFAULT, retryNavigation, uri, g_free);
    return G_SOURCE_CONTINUE;
}

static void navigationIssued(const char* kind, const char* uri, bool retry)
{
    g_free(pending.uri);
    pending.uri = g_strdup(uri && *uri ? uri : "about:blank");
    pending.kind = kind;
    pending.since = nowMs();
    pending.asked = pending.started = pending.reported = false;
    pending.questions = 0;
    pending.retry = retry;
    if (!pending.timer)
        pending.timer = g_timeout_add_seconds(1, pendingCheck, nullptr);
}

/* --- addresses ------------------------------------------------------------------------------ */

/* What the address field (or the command line) means: a URI as is, a path as a file, a host
 * name (it has a dot, or a port) over https, anything else -- one word, or several -- a search. */
static char* resolveInput(const char* input)
{
    g_autofree char* text = g_strstrip(g_strdup(input));
    if (!*text)
        return nullptr;
    if (strstr(text, "://") || g_str_has_prefix(text, "about:") || g_str_has_prefix(text, "data:"))
        return g_strdup(text);
    if (text[0] == '/')
        return g_filename_to_uri(text, nullptr, nullptr);
    if (g_str_has_prefix(text, "~/"))
        return g_strconcat("file://", g_get_home_dir(), text + 1, nullptr);

    bool hostLike = !strpbrk(text, " \t");
    size_t hostLength = strcspn(text, "/?#");
    g_autofree char* host = g_strndup(text, hostLength);
    if (hostLike) {
        const char* colon = strrchr(host, ':');
        bool port = colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1);
        bool dotted = strchr(host, '.') && host[0] != '.' && host[strlen(host) - 1] != '.';
        hostLike = dotted || port || !strcmp(host, "localhost");
    }
    if (hostLike) {
        /* the local network has no certificates */
        g_autofree char* name = g_strndup(host, strcspn(host, ":"));
        bool local = !strcmp(name, "localhost") || g_hostname_is_ip_address(name);
        return g_strconcat(local ? "http://" : "https://", text, nullptr);
    }
    g_autofree char* query = g_uri_escape_string(text, nullptr, FALSE);
    return g_strconcat(optSearch ? optSearch : defaultSearch, query, nullptr);
}

/* --- chrome: the toolbar overlay ------------------------------------------------------------ */

/*
 * Injected at document start into every top-level page, in the "wpe-browser" script world:
 * its JavaScript objects are invisible to the page and the page's Content-Security-Policy does
 * not apply to it. The DOM is shared, so everything lives in a closed shadow root under one
 * host element, styled through a constructed style sheet (no inline <style> for a page's CSP
 * to block). It draws what the UI process sends (wpeBrowserChrome.update) and posts the
 * buttons' actions back ("chrome" message handler); it never takes keyboard focus.
 */
static const char chromeScript[] = R"JS((() => {
    'use strict';
    if (window.top !== window || window.wpeBrowserChrome)
        return;
    const XHTML = 'http://www.w3.org/1999/xhtml';
    const CSS = `
        .bar { position: fixed; top: 0; left: 0; right: 0; height: 34px; box-sizing: border-box;
               display: none; align-items: center; gap: 4px; padding: 0 6px;
               background: #f6f5f4; color: #241f31; border-bottom: 1px solid #c0bfbc;
               box-shadow: 0 1px 4px rgba(0, 0, 0, .25); font: 14px "DejaVu Sans", sans-serif; }
        .bar.shown { display: flex; }
        .btn { all: unset; display: inline-block; width: 28px; height: 26px; line-height: 26px; text-align: center;
               border-radius: 4px; font-size: 16px; }
        .btn:hover { background: #deddda; }
        .btn:disabled { color: #9a9996; background: none; }
        .url { flex: 1; height: 24px; line-height: 24px; padding: 0 8px; overflow: hidden;
               white-space: pre; text-overflow: ellipsis; background: #fff;
               border: 1px solid #c0bfbc; border-radius: 4px; }
        .url.editing { border-color: #3584e4; box-shadow: 0 0 0 1px #3584e4; text-overflow: clip; }
        .caret { display: inline-block; width: 1px; height: 16px; margin-bottom: -3px; background: #241f31; }
        .sel { background: #3584e4; color: #fff; }
        .progress { position: fixed; top: 0; left: 0; height: 3px; width: 0; display: none;
                    background: #3584e4; pointer-events: none; }
        .progress.shown { display: block; }
        .progress.pinned { top: 31px; }`;
    let host = null, ui = null, hover = false;
    let state = { editing: false, before: '', after: '', sel: false, uri: '', loading: false,
                  progress: 0, back: false, fwd: false, pinned: false };
    const post = (message) => {
        try { window.webkit.messageHandlers.chrome.postMessage(message); } catch (e) { }
    };
    const element = (tag, cls, parent, text) => {
        const e = document.createElementNS(XHTML, tag);
        if (cls) e.className = cls;
        if (text) e.textContent = text;
        if (parent) parent.appendChild(e);
        return e;
    };
    const build = () => {
        if (!document.documentElement)
            return false;
        host = element('div');
        host.style.cssText = 'all: initial; position: fixed; top: 0; left: 0; width: 0; height: 0; z-index: 2147483647;';
        const root = host.attachShadow({ mode: 'closed' });
        const sheet = new CSSStyleSheet();
        sheet.replaceSync(CSS);
        root.adoptedStyleSheets = [sheet];
        ui = { bar: element('div', 'bar', root) };
        for (const [action, label, title] of [['back', '←', 'Back (Alt+Left)'],
            ['forward', '→', 'Forward (Alt+Right)'], ['reload', '↻', 'Reload (F5) / Stop (Esc)'],
            ['home', '⌂', 'Start page (Alt+Home)']]) {
            const b = element('button', 'btn', ui.bar, label);
            b.title = title;
            b.addEventListener('click', (ev) => {
                ev.preventDefault();
                ev.stopPropagation();
                post(action === 'reload' && state.loading ? 'stop' : action);
            });
            ui[action] = b;
        }
        ui.url = element('div', 'url', ui.bar);
        ui.url.title = 'Address (Ctrl+L)';
        ui.url.addEventListener('mousedown', (ev) => {
            ev.preventDefault();
            ev.stopPropagation();
            post('focus-url');
        });
        ui.bar.addEventListener('mouseleave', () => { hover = false; render(); });
        ui.progress = element('div', 'progress', root);
        document.documentElement.appendChild(host);
        return true;
    };
    const render = () => {
        if ((!host || !host.isConnected) && !build())
            return;
        ui.bar.classList.toggle('shown', state.pinned || state.editing || hover);
        ui.progress.classList.toggle('shown', state.loading);
        ui.progress.classList.toggle('pinned', state.pinned);
        ui.progress.style.width = Math.round(Math.max(0.05, state.progress) * 100) + '%';
        ui.back.disabled = !state.back;
        ui.forward.disabled = !state.fwd;
        ui.reload.textContent = state.loading ? '✕' : '↻';
        ui.url.classList.toggle('editing', state.editing);
        ui.url.textContent = '';
        if (!state.editing)
            ui.url.textContent = state.uri;
        else if (state.sel)
            element('span', 'sel', ui.url, state.before + state.after);
        else {
            element('span', null, ui.url, state.before);
            element('span', 'caret', ui.url);
            element('span', null, ui.url, state.after);
        }
    };
    window.wpeBrowserChrome = { update: (s) => { Object.assign(state, s); render(); } };
    document.addEventListener('mousemove', (ev) => {
        if (!hover && ev.clientY <= 3) {
            hover = true;
            render();
        }
    }, { capture: true, passive: true });
    post('ready');
})();)JS";

struct ChromeState {
    bool enabled { false };
    bool pinned { false }; /* --toolbar=always */
    bool editing { false };
    bool selectAll { false };
    std::u32string text;
    size_t caret { 0 };
};
static ChromeState chrome;

static std::string utf8(const std::u32string& text, size_t from, size_t to)
{
    if (from >= to)
        return { };
    g_autofree char* s = g_ucs4_to_utf8(reinterpret_cast<const gunichar*>(text.data() + from), static_cast<glong>(to - from),
        nullptr, nullptr, nullptr);
    return s ? s : "";
}

static void appendJSONString(GString* json, const char* key, const char* value)
{
    g_string_append_printf(json, "\"%s\":\"", key);
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value ? value : ""); *p; ++p) {
        if (*p == '"' || *p == '\\')
            g_string_append_printf(json, "\\%c", *p);
        else if (*p < 0x20)
            g_string_append_printf(json, "\\u%04x", *p);
        else
            g_string_append_c(json, static_cast<char>(*p));
    }
    g_string_append(json, "\",");
}

/* Send the whole state to the overlay. Before the page's overlay exists (between a commit and
 * the document start) the call finds nothing and does nothing; the script's "ready" message
 * asks for the state again. */
static void pushChrome()
{
    if (!chrome.enabled)
        return;
    GString* js = g_string_new("window.wpeBrowserChrome && wpeBrowserChrome.update({");
    appendJSONString(js, "before", utf8(chrome.text, 0, chrome.caret).c_str());
    appendJSONString(js, "after", utf8(chrome.text, chrome.caret, chrome.text.size()).c_str());
    appendJSONString(js, "uri", webkit_web_view_get_uri(webView));
    g_string_append_printf(js, "\"pinned\":%s,", chrome.pinned ? "true" : "false");
    g_string_append_printf(js, "\"editing\":%s,\"sel\":%s,\"loading\":%s,\"progress\":%.2f,\"back\":%s,\"fwd\":%s})",
        chrome.editing ? "true" : "false", chrome.selectAll ? "true" : "false",
        webkit_web_view_is_loading(webView) ? "true" : "false", webkit_web_view_get_estimated_load_progress(webView),
        webkit_web_view_can_go_back(webView) ? "true" : "false", webkit_web_view_can_go_forward(webView) ? "true" : "false");
    webkit_web_view_evaluate_javascript(webView, js->str, static_cast<gssize>(js->len), chromeWorld, nullptr, nullptr, nullptr, nullptr);
    g_string_free(js, TRUE);
}

static void loadAddress(const char* uri)
{
    if (!uri || !*uri)
        return;
    navigationIssued("load", uri);
    webkit_web_view_load_uri(webView, uri);
}

static const char* historyURI(WebKitBackForwardListItem* item)
{
    return item ? webkit_back_forward_list_item_get_uri(item) : nullptr;
}

static void startEditing(const char* source)
{
    const char* uri = webkit_web_view_get_uri(webView);
    g_autofree gunichar* ucs4 = g_utf8_to_ucs4(uri ? uri : "", -1, nullptr, nullptr, nullptr);
    chrome.text = ucs4 ? std::u32string(reinterpret_cast<const char32_t*>(ucs4)) : std::u32string();
    chrome.caret = chrome.text.size();
    chrome.selectAll = true;
    chrome.editing = true;
    LOG("chrome action=focus-url source=%s", source);
    pushChrome();
}

static void stopEditing(const char* why)
{
    if (!chrome.editing)
        return;
    chrome.editing = false;
    LOG("chrome action=cancel source=%s", why);
    pushChrome();
}

static void commitAddress(const char* source)
{
    std::string input = utf8(chrome.text, 0, chrome.text.size());
    g_autofree char* uri = resolveInput(input.c_str());
    chrome.editing = false;
    LOG("chrome action=go source=%s input=%s uri=%s", source, input.c_str(), uri ? uri : "(none)");
    loadAddress(uri);
    pushChrome();
}

/* the navigation actions, from a key, an overlay button, the --auto steps or the cycle */
static void chromeAction(const char* action, const char* source)
{
    if (!strcmp(action, "back")) {
        bool ok = webkit_web_view_can_go_back(webView);
        LOG("chrome action=back source=%s ok=%d", source, ok);
        if (ok) {
            navigationIssued("back", historyURI(webkit_back_forward_list_get_back_item(webkit_web_view_get_back_forward_list(webView))));
            webkit_web_view_go_back(webView);
        }
    } else if (!strcmp(action, "forward")) {
        bool ok = webkit_web_view_can_go_forward(webView);
        LOG("chrome action=forward source=%s ok=%d", source, ok);
        if (ok) {
            navigationIssued("forward", historyURI(webkit_back_forward_list_get_forward_item(webkit_web_view_get_back_forward_list(webView))));
            webkit_web_view_go_forward(webView);
        }
    } else if (!strcmp(action, "reload")) {
        LOG("chrome action=reload source=%s uri=%s", source, webkit_web_view_get_uri(webView));
        navigationIssued("reload", webkit_web_view_get_uri(webView));
        webkit_web_view_reload(webView);
    } else if (!strcmp(action, "reload-nocache")) {
        LOG("chrome action=reload-nocache source=%s uri=%s", source, webkit_web_view_get_uri(webView));
        navigationIssued("reload", webkit_web_view_get_uri(webView));
        webkit_web_view_reload_bypass_cache(webView);
    } else if (!strcmp(action, "stop")) {
        LOG("chrome action=stop source=%s loading=%d", source, webkit_web_view_is_loading(webView));
        pendingDone();
        webkit_web_view_stop_loading(webView);
    } else if (!strcmp(action, "home")) {
        LOG("chrome action=home source=%s uri=%s", source, homeURI);
        loadAddress(homeURI);
    } else if (!strcmp(action, "focus-url"))
        startEditing(source);
    else
        LOG("chrome unknown-action %s source=%s", action, source);
}

static void chromeMessage(WebKitUserContentManager*, JSCValue* value, gpointer)
{
    g_autofree char* message = jsc_value_to_string(value);
    if (!g_strcmp0(message, "ready"))
        pushChrome();
    else if (message)
        chromeAction(message, "ui");
}

#if ENABLE_MEDIA_SOURCE
/* --mse as the pages see it: at every top-frame load a script in a world of its own reports
 * typeof MediaSource and typeof ManagedMediaSource ("media mse-check ... result=ok|MISMATCH").
 * Build 59's --mse=off logged its setting and still gave pages a MediaSource (WebCore reset the
 * preference at each load; patch 0032 fixes that): the setting is checked where it matters. */
static const char* mseMode = "on";
static constexpr const char* mediaCheckWorld = "wpe-browser-media-check";

static void mediaCheckMessage(WebKitUserContentManager*, JSCValue* value, gpointer)
{
    g_autofree char* message = jsc_value_to_string(value);
    char mediaSource[32] = "", managed[32] = "";
    if (!message || sscanf(message, "%31s %31s", mediaSource, managed) != 2)
        return;
    bool hasMediaSource = strcmp(mediaSource, "undefined");
    bool hasManaged = strcmp(managed, "undefined");
    bool ok = hasMediaSource == !!strcmp(mseMode, "off") && hasManaged == !strcmp(mseMode, "managed");
    LOG("media mse-check mse=%s MediaSource=%s ManagedMediaSource=%s result=%s uri=%s", mseMode, hasMediaSource ? "present" : "absent",
        hasManaged ? "present" : "absent", ok ? "ok" : "MISMATCH", webView ? webkit_web_view_get_uri(webView) : "-");
}
#endif

/* The address line editor: every key while editing stays in the UI process. */
static void editAddress(guint keyval, WPEModifiers modifiers, const char* source)
{
    const bool control = modifiers & WPE_MODIFIER_KEYBOARD_CONTROL;
    auto replaceSelection = [] {
        if (chrome.selectAll) {
            chrome.text.clear();
            chrome.caret = 0;
            chrome.selectAll = false;
        }
    };
    switch (keyval) {
    case WPE_KEY_Return:
    case WPE_KEY_KP_Enter:
        commitAddress(source);
        return;
    case WPE_KEY_Escape:
        stopEditing(source);
        return;
    case WPE_KEY_BackSpace:
        if (chrome.selectAll)
            replaceSelection();
        else if (chrome.caret)
            chrome.text.erase(--chrome.caret, 1);
        break;
    case WPE_KEY_Delete:
    case WPE_KEY_KP_Delete:
        if (chrome.selectAll)
            replaceSelection();
        else if (chrome.caret < chrome.text.size())
            chrome.text.erase(chrome.caret, 1);
        break;
    case WPE_KEY_Left:
    case WPE_KEY_KP_Left:
        chrome.caret = chrome.selectAll ? 0 : (chrome.caret ? chrome.caret - 1 : 0);
        chrome.selectAll = false;
        break;
    case WPE_KEY_Right:
    case WPE_KEY_KP_Right:
        chrome.caret = chrome.selectAll ? chrome.text.size() : std::min(chrome.caret + 1, chrome.text.size());
        chrome.selectAll = false;
        break;
    case WPE_KEY_Home:
    case WPE_KEY_KP_Home:
        chrome.caret = 0;
        chrome.selectAll = false;
        break;
    case WPE_KEY_End:
    case WPE_KEY_KP_End:
        chrome.caret = chrome.text.size();
        chrome.selectAll = false;
        break;
    default:
        if (control && (keyval == WPE_KEY_a || keyval == WPE_KEY_l))
            chrome.selectAll = true;
        else if (control && keyval == WPE_KEY_u) {
            chrome.selectAll = true;
            replaceSelection();
        } else if (!control && !(modifiers & WPE_MODIFIER_KEYBOARD_ALT)) {
            gunichar c = wpe_keyval_to_unicode(keyval);
            if (c < 0x20 || c == 0x7f)
                return;
            replaceSelection();
            chrome.text.insert(chrome.caret++, 1, static_cast<char32_t>(c));
        } else
            return;
    }
    pushChrome();
}

/* --- page events ---------------------------------------------------------------------------- */

static guint32 crc32Update(guint32 crc, const guint8* data, gsize length)
{
    crc = ~crc;
    for (gsize i = 0; i < length; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

/* WebKitImage: 32-bit BGRA (little-endian), premultiplied alpha. PNG wants RGBA, straight. */
static bool writePNG(const char* path, WebKitImage* image, guint32* crcOut)
{
    int width = webkit_image_get_width(image);
    int height = webkit_image_get_height(image);
    guint stride = webkit_image_get_stride(image);
    GBytes* bytes = webkit_image_as_bytes(image); /* (transfer none): the image keeps it */
    gsize size;
    const guint8* pixels = static_cast<const guint8*>(g_bytes_get_data(bytes, &size));

    FILE* file = fopen(path, "wb");
    if (!file) {
        LOG("snapshot-error open %s: %s", path, strerror(errno));
        return false;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    guint8* row = static_cast<guint8*>(g_malloc(static_cast<gsize>(width) * 4));
    guint32 crc = 0;
    bool ok = false;
    if (png && info && !setjmp(png_jmpbuf(png))) {
        png_init_io(png, file);
        png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
            PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        for (int y = 0; y < height; y++) {
            const guint8* src = pixels + static_cast<gsize>(y) * stride;
            for (int x = 0; x < width; x++) {
                guint8 b = src[4 * x], g = src[4 * x + 1], r = src[4 * x + 2], a = src[4 * x + 3];
                if (a && a != 255) {
                    r = static_cast<guint8>(r * 255 / a);
                    g = static_cast<guint8>(g * 255 / a);
                    b = static_cast<guint8>(b * 255 / a);
                }
                row[4 * x] = r;
                row[4 * x + 1] = g;
                row[4 * x + 2] = b;
                row[4 * x + 3] = a;
            }
            crc = crc32Update(crc, row, static_cast<gsize>(width) * 4);
            png_write_row(png, row);
        }
        png_write_end(png, nullptr);
        ok = true;
    }
    png_destroy_write_struct(&png, &info);
    g_free(row);
    fclose(file);
    *crcOut = crc;
    return ok;
}

static void snapshotReady(GObject* object, GAsyncResult* result, gpointer)
{
    GError* error = nullptr;
    WebKitImage* image = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (!image) {
        LOG("snapshot-error %s", error ? error->message : "unknown");
        g_clear_error(&error);
        quit(1);
        return;
    }
    guint32 crc = 0;
    bool ok = writePNG(optSnapshot, image, &crc);
    if (ok)
        LOG("snapshot file=%s width=%d height=%d crc32=%08x", optSnapshot, webkit_image_get_width(image),
            webkit_image_get_height(image), crc);
    g_object_unref(image);
    quit(ok ? 0 : 1);
}

static const char* loadEventName(WebKitLoadEvent event)
{
    switch (event) {
    case WEBKIT_LOAD_STARTED:
        return "started";
    case WEBKIT_LOAD_REDIRECTED:
        return "redirected";
    case WEBKIT_LOAD_COMMITTED:
        return "committed";
    case WEBKIT_LOAD_FINISHED:
        return "finished";
    }
    return "?";
}

static void loadChanged(WebKitWebView* view, WebKitLoadEvent event, gpointer)
{
    LOG("load %s uri=%s", loadEventName(event), webkit_web_view_get_uri(view));
    if (event == WEBKIT_LOAD_STARTED)
        pending.started = true;
    else if (event == WEBKIT_LOAD_COMMITTED)
        pendingDone();
    pushChrome();
    if (event != WEBKIT_LOAD_FINISHED || firstLoadDone)
        return;
    firstLoadDone = TRUE;
    if (optSnapshot) {
        webkit_web_view_get_snapshot(view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
            nullptr, snapshotReady, nullptr);
        return;
    }
    if (optExitAfterLoad)
        quit(0);
}

static gboolean loadFailed(WebKitWebView*, WebKitLoadEvent, const char* uri, GError* error, gpointer)
{
    LOG("load-failed uri=%s error=%s page-id=%llu", uri, error ? error->message : "?", static_cast<unsigned long long>(pageID()));
    /* a load cancelled by the next one (the cycle, a new address) leaves that one pending */
    if (!g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
        pendingDone();
    return FALSE; /* WebKit's error page */
}

/* Our own page for a refused certificate: WebKit's says only "Unacceptable TLS certificate". The
 * Pi has no RTC, so the commonest cause is a clock that never got set (ntpclient found no server):
 * then every certificate is "not yet valid", and the page says what to do about it. */
static gboolean loadFailedTLS(WebKitWebView* view, const char* uri, GTlsCertificate*, GTlsCertificateFlags errors, gpointer)
{
    static const struct {
        GTlsCertificateFlags flag;
        const char* text;
    } reasons[] = {
        { G_TLS_CERTIFICATE_UNKNOWN_CA, "it is not signed by a known certificate authority" },
        { G_TLS_CERTIFICATE_BAD_IDENTITY, "it is not for this site's name" },
        { G_TLS_CERTIFICATE_NOT_ACTIVATED, "it is not valid yet" },
        { G_TLS_CERTIFICATE_EXPIRED, "it has expired" },
        { G_TLS_CERTIFICATE_REVOKED, "it has been revoked" },
        { G_TLS_CERTIFICATE_INSECURE, "its algorithm is insecure" },
    };
    /* 2024-01-01: no certificate a site serves today was issued before the clock could read this */
    const bool clockUnset = time(nullptr) < 1704067200;

    LOG("load-failed-tls uri=%s flags=0x%x clock=%s", uri, static_cast<unsigned>(errors), clockUnset ? "unset" : "set");
    pendingDone();

    char* site = g_markup_escape_text(uri ? uri : "", -1);
    GString* html = g_string_new(nullptr);
    g_string_append_printf(html, "<!DOCTYPE html><html><head><meta charset=utf-8><title>Certificate refused</title>"
        "<style>body{font:16px sans-serif;margin:2em;max-width:40em}code{background:#eee;padding:0 .2em}</style>"
        "</head><body><h1>Certificate refused</h1><p>The certificate of <b>%s</b> was not accepted:</p><ul>", site);
    for (const auto& reason : reasons) {
        if (errors & reason.flag)
            g_string_append_printf(html, "<li>%s</li>", reason.text);
    }
    g_string_append(html, "</ul>");
    if (clockUnset) {
        g_string_append(html, "<p><b>This system's clock is not set</b> (it reads 1970), so every certificate looks "
            "not valid yet. The board has no battery-backed clock and sets it from the network at boot; that "
            "failed. Run <code>ntpclient -w 30</code> in a terminal (or set another server in "
            "<code>/etc/ntp.conf</code>), then reload this page.</p>");
    }
    g_string_append(html, "<p>The page was not loaded, to keep the connection safe.</p></body></html>");
    webkit_web_view_load_alternate_html(view, html->str, uri, nullptr);
    g_string_free(html, TRUE);
    g_free(site);
    return TRUE;
}

static void webProcessTerminated(WebKitWebView* view, WebKitWebProcessTerminationReason reason, gpointer)
{
    LOG("web-process-terminated reason=%s page-id=%llu uri=%s", reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed"
        : reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "memory-limit" : "api",
        static_cast<unsigned long long>(pageID()), webkit_web_view_get_uri(view));
    if (!pending.retry)
        pendingDone(); /* the hang recovery's own termination keeps its second attempt */
    if (optSnapshot || optExitAfterLoad)
        quit(3);
}

/* WebKit moved the view to another web process: a process swap (its WebPage has a new id) */
static guint64 lastPageID;
static void pageIDChanged(WebKitWebView* view, GParamSpec*, gpointer)
{
    const guint64 id = webkit_web_view_get_page_id(view);
    LOG("page-swap page-id=%llu from=%llu pending_ms=%.0f uri=%s", static_cast<unsigned long long>(id),
        static_cast<unsigned long long>(lastPageID), pending.uri ? nowMs() - pending.since : -1.0, webkit_web_view_get_uri(view));
    lastPageID = id;
}

/* WebKit's verdict on the page's web process: 0 after 3 s without the answer to a message */
static void responsiveChanged(WebKitWebView* view, GParamSpec*, gpointer)
{
    LOG("web-process responsive=%d page-id=%llu loading=%d pending_ms=%.0f uri=%s", webkit_web_view_get_is_web_process_responsive(view),
        static_cast<unsigned long long>(webkit_web_view_get_page_id(view)), webkit_web_view_is_loading(view),
        pending.uri ? nowMs() - pending.since : -1.0, webkit_web_view_get_uri(view));
}

static void titleChanged(WebKitWebView* view, GParamSpec*, gpointer)
{
    const char* title = webkit_web_view_get_title(view);
    LOG("title %s", title ? title : "");
    if (WPEView* wpeView = webkit_web_view_get_wpe_view(view)) {
        if (WPEToplevel* toplevel = wpe_view_get_toplevel(wpeView))
            wpe_toplevel_set_title(toplevel, title && *title ? title : "wpe-browser");
    }
}

static void progressChanged(WebKitWebView* view, GParamSpec*, gpointer)
{
    LOG("progress %.2f", webkit_web_view_get_estimated_load_progress(view));
    pushChrome();
}

/*
 * There is one view: target=_blank links and window.open() load in it. The other decisions are
 * WebKit's defaults; two of them are logged, as the steps of a navigation: the question of the
 * page's current web process for a pending navigation ("policy navigation": the process is
 * alive and took the request) and the main resource's response ("policy response").
 */
static gboolean decidePolicy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer)
{
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        auto* response = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        if (webkit_response_policy_decision_is_main_frame_main_resource(response)) {
            WebKitURIResponse* r = webkit_response_policy_decision_get_response(response);
            LOG("policy response status=%u mime=%s page-id=%llu uri=%s", webkit_uri_response_get_status_code(r),
                webkit_uri_response_get_mime_type(r), static_cast<unsigned long long>(pageID()), webkit_uri_response_get_uri(r));
        }
        return FALSE;
    }
    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        /* main frames and subframes alike: the first few while a navigation waits for its start */
        if (pending.uri && !pending.started && pending.questions < 3) {
            pending.questions++;
            const bool ours = !g_strcmp0(uri, pending.uri);
            pending.asked = pending.asked || ours;
            LOG("policy navigation wait_ms=%.0f ours=%d redirect=%d page-id=%llu uri=%s", nowMs() - pending.since, ours,
                webkit_navigation_action_is_redirect(action), static_cast<unsigned long long>(pageID()), uri ? uri : "");
        }
        return FALSE;
    }
    LOG("new-window uri=%s opened=same-view via=policy", uri ? uri : "");
    webkit_policy_decision_ignore(decision);
    if (uri && *uri && strcmp(uri, "about:blank"))
        loadAddress(uri);
    return TRUE;
}

static WebKitWebView* createView(WebKitWebView*, WebKitNavigationAction* action, gpointer)
{
    const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    LOG("new-window uri=%s opened=same-view via=create", uri ? uri : "");
    if (uri && *uri && strcmp(uri, "about:blank"))
        loadAddress(uri);
    return nullptr;
}

/* --- keys ----------------------------------------------------------------------------------- */

static bool shortcut(WPEView* view, guint keyval, WPEModifiers modifiers, const char* source)
{
    const bool control = modifiers & WPE_MODIFIER_KEYBOARD_CONTROL;
    const bool alt = modifiers & WPE_MODIFIER_KEYBOARD_ALT;
    const bool shift = modifiers & WPE_MODIFIER_KEYBOARD_SHIFT;
    if (control && (keyval == WPE_KEY_q || keyval == WPE_KEY_Q)) {
        LOG("chrome action=quit source=%s", source);
        quit(0);
    } else if (control && (keyval == WPE_KEY_r || keyval == WPE_KEY_R))
        chromeAction(shift ? "reload-nocache" : "reload", source);
    else if (keyval == WPE_KEY_F5)
        chromeAction(shift || control ? "reload-nocache" : "reload", source);
    else if (chrome.enabled && ((control && (keyval == WPE_KEY_l || keyval == WPE_KEY_L)) || (alt && (keyval == WPE_KEY_d || keyval == WPE_KEY_D)) || keyval == WPE_KEY_F6))
        chromeAction("focus-url", source);
    else if (alt && (keyval == WPE_KEY_Left || keyval == WPE_KEY_KP_Left))
        chromeAction("back", source);
    else if (alt && (keyval == WPE_KEY_Right || keyval == WPE_KEY_KP_Right))
        chromeAction("forward", source);
    else if (alt && (keyval == WPE_KEY_Home || keyval == WPE_KEY_KP_Home))
        chromeAction("home", source);
    else if (keyval == WPE_KEY_Escape && webkit_web_view_is_loading(webView))
        chromeAction("stop", source);
    else if (keyval == WPE_KEY_F11) {
        WPEToplevel* toplevel = wpe_view_get_toplevel(view);
        if (!toplevel)
            return false;
        bool full = wpe_toplevel_get_state(toplevel) & WPE_TOPLEVEL_STATE_FULLSCREEN;
        LOG("chrome action=%s source=%s", full ? "unfullscreen" : "fullscreen", source);
        if (full)
            wpe_toplevel_unfullscreen(toplevel);
        else
            wpe_toplevel_fullscreen(toplevel);
    } else
        return false;
    return true;
}

static const char* eventSource = "key"; /* "auto" while an --auto step feeds its events */

static gboolean viewEvent(WPEView* view, WPEEvent* event, gpointer)
{
    WPEEventType type = wpe_event_get_event_type(event);
    if (type == WPE_EVENT_POINTER_DOWN) {
        /* a click into the page ends address editing (the overlay's own field posts focus-url) */
        stopEditing("pointer");
        return FALSE;
    }
    if (type != WPE_EVENT_KEYBOARD_KEY_DOWN && type != WPE_EVENT_KEYBOARD_KEY_UP)
        return FALSE;
    if (chrome.editing) {
        if (type == WPE_EVENT_KEYBOARD_KEY_DOWN)
            editAddress(wpe_event_keyboard_get_keyval(event), wpe_event_get_modifiers(event), eventSource);
        return TRUE; /* the page sees none of it */
    }
    if (type != WPE_EVENT_KEYBOARD_KEY_DOWN)
        return FALSE;
    return shortcut(view, wpe_event_keyboard_get_keyval(event), wpe_event_get_modifiers(event), eventSource);
}

/* --- test knobs: --auto, --cycle, --rss-secs ------------------------------------------------ */

/* "ctrl+shift+r", "alt+Left", "F5", "Return", "x" */
static bool parseKeys(const char* spec, guint* keyval, WPEModifiers* modifiers)
{
    static const struct {
        const char* name;
        guint keyval;
    } names[] = {
        { "Return", WPE_KEY_Return }, { "Escape", WPE_KEY_Escape }, { "BackSpace", WPE_KEY_BackSpace },
        { "Delete", WPE_KEY_Delete }, { "Tab", WPE_KEY_Tab }, { "Left", WPE_KEY_Left }, { "Right", WPE_KEY_Right },
        { "Up", WPE_KEY_Up }, { "Down", WPE_KEY_Down }, { "Home", WPE_KEY_Home }, { "End", WPE_KEY_End },
        { "Page_Up", WPE_KEY_Page_Up }, { "Page_Down", WPE_KEY_Page_Down }, { "space", WPE_KEY_space },
        { "F5", WPE_KEY_F5 }, { "F6", WPE_KEY_F6 }, { "F11", WPE_KEY_F11 },
    };
    g_auto(GStrv) parts = g_strsplit(spec, "+", -1);
    guint n = g_strv_length(parts);
    if (!n)
        return false;
    unsigned mods = 0;
    for (guint i = 0; i + 1 < n; i++) {
        if (!g_ascii_strcasecmp(parts[i], "ctrl"))
            mods |= WPE_MODIFIER_KEYBOARD_CONTROL;
        else if (!g_ascii_strcasecmp(parts[i], "alt"))
            mods |= WPE_MODIFIER_KEYBOARD_ALT;
        else if (!g_ascii_strcasecmp(parts[i], "shift"))
            mods |= WPE_MODIFIER_KEYBOARD_SHIFT;
        else
            return false;
    }
    const char* key = parts[n - 1];
    *keyval = 0;
    for (const auto& entry : names) {
        if (!strcmp(key, entry.name))
            *keyval = entry.keyval;
    }
    if (!*keyval && g_utf8_strlen(key, -1) == 1)
        *keyval = wpe_unicode_to_keyval(g_utf8_get_char(key));
    *modifiers = static_cast<WPEModifiers>(mods);
    return *keyval;
}

static void feedKey(guint keyval, WPEModifiers modifiers)
{
    WPEView* view = webkit_web_view_get_wpe_view(webView);
    if (!view)
        return;
    eventSource = "auto";
    guint32 time = static_cast<guint32>(g_get_monotonic_time() / 1000);
    for (WPEEventType type : { WPE_EVENT_KEYBOARD_KEY_DOWN, WPE_EVENT_KEYBOARD_KEY_UP }) {
        WPEEvent* event = wpe_event_keyboard_new(type, view, WPE_INPUT_SOURCE_KEYBOARD, time, modifiers, 0, keyval);
        wpe_view_event(view, event);
        wpe_event_unref(event);
    }
    eventSource = "key";
}

static gboolean autoStep(gpointer data)
{
    const char* step = static_cast<const char*>(data); /* "key:<keys>" or "type:<text>" */
    if (g_str_has_prefix(step, "key:")) {
        guint keyval;
        WPEModifiers modifiers;
        if (!parseKeys(step + 4, &keyval, &modifiers)) {
            LOG("auto bad-keys %s", step + 4);
            return G_SOURCE_REMOVE;
        }
        LOG("auto key=%s", step + 4);
        feedKey(keyval, modifiers);
    } else if (g_str_has_prefix(step, "type:")) {
        LOG("auto type=%s", step + 5);
        for (const char* p = step + 5; *p; p = g_utf8_next_char(p))
            feedKey(wpe_unicode_to_keyval(g_utf8_get_char(p)), static_cast<WPEModifiers>(0));
    } else
        LOG("auto bad-step %s", step);
    return G_SOURCE_REMOVE;
}

/* <seconds>:key:<keys> | <seconds>:type:<text>, comma-separated; seconds from the start */
static void scheduleAuto(const char* spec)
{
    g_auto(GStrv) steps = g_strsplit(spec, ",", -1);
    for (char** s = steps; *s; s++) {
        char* rest;
        double at = g_ascii_strtod(*s, &rest);
        if (rest == *s || *rest != ':') {
            LOG("auto bad-step %s", *s);
            continue;
        }
        g_timeout_add_full(G_PRIORITY_DEFAULT, static_cast<guint>(at * 1000), autoStep, g_strdup(rest + 1), g_free);
    }
}

static GPtrArray* cyclePages;
static guint cycleNextIndex;

static gboolean cycleNext(gpointer)
{
    const char* uri = static_cast<const char*>(g_ptr_array_index(cyclePages, cycleNextIndex % cyclePages->len));
    /* the state the previous navigation left: a still pending one never committed */
    LOG("cycle n=%u loading=%d responsive=%d page-id=%llu pending_ms=%.0f uri=%s", ++cycleNextIndex, webkit_web_view_is_loading(webView),
        webkit_web_view_get_is_web_process_responsive(webView), static_cast<unsigned long long>(pageID()),
        pending.uri ? nowMs() - pending.since : -1.0, uri);
    stopEditing("cycle");
    loadAddress(uri);
    return G_SOURCE_CONTINUE;
}

/* a file with one entry per line ('#' comments), or a comma-separated list */
static void loadCycle(const char* list)
{
    g_autofree char* contents = nullptr;
    bool file = list[0] == '/' && g_file_get_contents(list, &contents, nullptr, nullptr);
    g_auto(GStrv) entries = g_strsplit(file ? contents : list, file ? "\n" : ",", -1);
    cyclePages = g_ptr_array_new_with_free_func(g_free);
    for (char** e = entries; *e; e++) {
        g_strstrip(*e);
        if (**e && **e != '#') {
            if (char* uri = resolveInput(*e))
                g_ptr_array_add(cyclePages, uri);
        }
    }
    LOG("cycle pages=%u secs=%d from=%s", cyclePages->len, optCycleSecs, file ? list : "list");
}

/* the kernel's page allocator: the RAM really in use, which the per-process footprints are not
 * (a map entry's anonymous pages count its whole amap, so entries split from one mapping count it
 * again and again: kernel vm/map.c, meminfo) */
static void logSystemMemory()
{
    meminfo_t info;
    memset(&info, 0, sizeof(info));
    info.page.mapsz = -1;
    info.entry.mapsz = -1;
    info.entry.kmapsz = -1;
    info.maps.mapsz = -1;
    meminfo(&info);
    LOG("sysmem used_kb=%u free_kb=%u", info.page.alloc / 1024, info.page.free / 1024);
}

static gboolean logUIFootprint(gpointer)
{
    logFootprint("ui");
    logSystemMemory();
    return G_SOURCE_CONTINUE;
}

/* --- GPU: the frame transport, WebGL, the presented frames (browser milestone B7) -------------- */

static constexpr guint32 fourccABGR8888 = 0x34324241; /* drm_fourcc.h DRM_FORMAT_ABGR8888, 'AB24' */

/*
 * The frame transport. By default (--dmabuf; WebKit patch 0016: WPE_PHOENIX_DMABUF=1, read by the
 * UI process when it starts a web process) each render target is a GL texture exported as a
 * dma-buf and handed to the compositor through zwp_linux_dmabuf_v1: no readback, no copy. Window
 * mode only, and only when the compositor offers linux-dmabuf: a headless view takes snapshots,
 * and without GBM WebKit cannot read a dma-buf back for one. Otherwise (--shm) the web process
 * renders with GLES (EGL surfaceless on the V3D) and reads every frame back into shared memory,
 * which the UI process copies once more into a wl_shm pool for labwc.
 */
static const char* chooseFrameTransport(WPEDisplay* display)
{
    if (!optDMABuf)
        return "shm";
    if (optHeadless) {
        if (dmabufAsked)
            LOG("gpu dmabuf-refused reason=headless (snapshots read SHM frames only)");
        return "shm";
    }
    WPEBufferFormats* formats = wpe_display_get_preferred_buffer_formats(display); /* transfer none */
    if (!formats) {
        LOG("gpu dmabuf-refused reason=no-linux-dmabuf (the compositor does not offer zwp_linux_dmabuf_v1)");
        return "shm";
    }
    /* what the compositor advertised (empty with a v4 linux-dmabuf: WPEPlatform asks for its
     * feedback only with libdrm). The web process exports what the V3D renders (ABGR8888, UIF). */
    guint n = 0;
    GString* abgr = g_string_new(nullptr);
    for (guint g = 0; g < wpe_buffer_formats_get_n_groups(formats); g++) {
        for (guint f = 0; f < wpe_buffer_formats_get_group_n_formats(formats, g); f++, n++) {
            if (wpe_buffer_formats_get_format_fourcc(formats, g, f) != fourccABGR8888)
                continue;
            GArray* modifiers = wpe_buffer_formats_get_format_modifiers(formats, g, f);
            for (guint m = 0; modifiers && m < modifiers->len; m++)
                g_string_append_printf(abgr, "%s0x%016" G_GINT64_MODIFIER "x", abgr->len ? "," : "", g_array_index(modifiers, guint64, m));
        }
    }
    LOG("gpu dmabuf-formats n=%u abgr8888=%s", n, abgr->len ? abgr->str : "-");
    g_string_free(abgr, TRUE);
    g_setenv("WPE_PHOENIX_DMABUF", "1", TRUE);
    return "dmabuf";
}

/* --present-stats: the frames the view put on screen (WPEView::buffer-rendered, the signal WebKit's
 * backing store answers with a frame-done), and what they were made of */
static struct {
    unsigned frames;
    unsigned total;
    double since;
    const char* buffer;
    int width, height;
} present;

static void presentBufferRendered(WPEView*, WPEBuffer* buffer, gpointer)
{
    present.frames++;
    present.total++;
    present.buffer = WPE_IS_BUFFER_DMA_BUF(buffer) ? "dma-buf" : WPE_IS_BUFFER_SHM(buffer) ? "shm" : "other";
    present.width = wpe_buffer_get_width(buffer);
    present.height = wpe_buffer_get_height(buffer);
}

/* every presented frame, for the watchdog's --frame-stall-secs */
static void viewBufferRendered(WPEView*, WPEBuffer*, gpointer)
{
    presentedTotal.fetch_add(1);
    presentedLastMs.store(static_cast<int64_t>(nowMs()));
}

static gboolean presentReport(gpointer)
{
    double now = nowMs();
    double secs = (now - present.since) / 1000;
    LOG("present frames=%u fps=%.1f total=%u buffer=%s size=%dx%d", present.frames, secs > 0 ? present.frames / secs : 0.0,
        present.total, present.buffer ? present.buffer : "none", present.width, present.height);
    present.frames = 0;
    present.since = now;
    return G_SOURCE_CONTINUE;
}

/* --- network session -------------------------------------------------------------------------- */

static char* storageDirectory(const char* option, const char* first, const char* second)
{
    if (option)
        return g_strdup(option);
    /* $HOME, not XDG_DATA_HOME/XDG_CACHE_HOME: the XFCE session points those at its RAM /tmp */
    const char* home = g_getenv("HOME");
    const char* base = home && *home ? home : "/root";
    /* g_build_filename() stops at the first NULL: with no second component, "wpe-browser" must
     * follow `first` directly (the cache was created as $HOME/.cache itself) */
    if (!second)
        return g_build_filename(base, first, "wpe-browser", nullptr);
    return g_build_filename(base, first, second, "wpe-browser", nullptr);
}

static WebKitNetworkSession* createNetworkSession()
{
    if (optEphemeral || optHeadless) {
        /* WebKit's FrameConsoleClient prints no console message of a page in an ephemeral session,
         * whatever enable-write-console-messages-to-stdout says: a check that reads the page's
         * console.log lines needs a persistent session (--data-dir/--cache-dir in /tmp) */
        LOG("session ephemeral console=off");
        return webkit_network_session_new_ephemeral();
    }
    g_autofree char* dataDir = storageDirectory(optDataDir, ".local", "share");
    g_autofree char* cacheDir = storageDirectory(optCacheDir, ".cache", nullptr);
    for (const char* dir : { static_cast<const char*>(dataDir), static_cast<const char*>(cacheDir) }) {
        if (g_mkdir_with_parents(dir, 0700) < 0) {
            LOG("session-error mkdir %s: %s; falling back to an ephemeral session", dir, g_strerror(errno));
            return webkit_network_session_new_ephemeral();
        }
    }
    /* the steps a start-up stall report tells apart: the WebsiteDataStore (it waits for its
     * WorkQueue threads to start), the NetworkProcess launch (the cookie manager starts
     * observing the cookie store; posix_spawn() returns once the child has exec()ed), and the
     * cookie settings (its first messages, which wait in a queue until it has started) */
    startupPhase("session-new");
    WebKitNetworkSession* session = webkit_network_session_new(dataDir, cacheDir);
    g_autofree char* cookies = g_build_filename(dataDir, "cookies.sqlite", nullptr);
    startupPhase("network-launch");
    WebKitCookieManager* cookieManager = webkit_network_session_get_cookie_manager(session);
    startupPhase("cookie-settings");
    webkit_cookie_manager_set_persistent_storage(cookieManager, cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    webkit_cookie_manager_set_accept_policy(cookieManager, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
    /* cache-model: set with the web context (createWebContext()) */
    LOG("session persistent data=%s cache=%s cookies=%s cookie-policy=no-third-party cache-model=web-browser",
        dataDir, cacheDir, cookies);
    return session;
}

/*
 * The web context: WebKit's process pool, the web processes' settings. Made after the network
 * session, in MiniBrowser's order (Tools/MiniBrowser/wpe/main.cpp: the session and its cookie
 * settings, then the context): the b29 soak made it first (for --web-extensions) and its UI never
 * got past the network session that followed. Nothing in WebKit makes that order wait (every
 * message a session sends to a NetworkProcess that is still launching is queued, and a sync one
 * fails at once), so that stall is the watchdog's to explain; this order is the one every working
 * run used. The extensions only have to be set before the first web process starts, which is
 * the web view's first load.
 */
static WebKitWebContext* createWebContext()
{
    /* --memory-limit/-kill/-poll-secs: the settings are a construct property, so a context of our
     * own (the web view is given it); otherwise WebKit's default one */
    WebKitWebContext* webContext;
    if (optMemoryLimitMB > 0 || optMemoryKill > 0 || optMemoryPollSecs > 0) {
        WebKitMemoryPressureSettings* memory = webkit_memory_pressure_settings_new();
        if (optMemoryLimitMB > 0)
            webkit_memory_pressure_settings_set_memory_limit(memory, static_cast<guint>(optMemoryLimitMB));
        if (optMemoryKill > 0) {
            if (optMemoryKill > webkit_memory_pressure_settings_get_strict_threshold(memory))
                webkit_memory_pressure_settings_set_kill_threshold(memory, optMemoryKill);
            else
                LOG("memory-pressure kill=%.2f ignored: it must be above the strict threshold %.2f", optMemoryKill,
                    webkit_memory_pressure_settings_get_strict_threshold(memory));
        }
        if (optMemoryPollSecs > 0)
            webkit_memory_pressure_settings_set_poll_interval(memory, optMemoryPollSecs);
        LOG("memory-pressure limit_mb=%u conservative=%.2f strict=%.2f kill=%.2f poll_s=%.1f",
            webkit_memory_pressure_settings_get_memory_limit(memory), webkit_memory_pressure_settings_get_conservative_threshold(memory),
            webkit_memory_pressure_settings_get_strict_threshold(memory), webkit_memory_pressure_settings_get_kill_threshold(memory),
            webkit_memory_pressure_settings_get_poll_interval(memory));
        webContext = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", memory, nullptr));
        webkit_memory_pressure_settings_free(memory);
    } else
        webContext = webkit_web_context_get_default();
    /* the default, but the disk cache depends on it */
    webkit_web_context_set_cache_model(webContext, WEBKIT_CACHE_MODEL_WEB_BROWSER);
    /* The WebProcess's injected bundle (libWPEInjectedBundle.so, dlopen()ed) loads these. */
    if (optWebExtensions) {
        webkit_web_context_set_web_process_extensions_directory(webContext, optWebExtensions);
        webkit_web_context_set_web_process_extensions_initialization_user_data(webContext, g_variant_new_string("wpe-browser"));
        LOG("web-extensions dir=%s", optWebExtensions);
    }
    return webContext;
}

/* --- UI role: main ---------------------------------------------------------------------------- */

static void displayDisconnected(WPEDisplay*, GError* error, gpointer)
{
    LOG("display-disconnected %s", error ? error->message : "");
    quit(1);
}

static gboolean loadTimeout(gpointer)
{
    if (!firstLoadDone) {
        LOG("timeout after %d s", optTimeout);
        quit(2);
    }
    return G_SOURCE_REMOVE;
}

static gboolean quitOnSignal(gpointer)
{
    LOG("signal quit");
    quit(0);
    return G_SOURCE_REMOVE;
}

/* the first page: the argument (a file, a URI, a host or search words), else the start page */
static char* initialURI(const char* arg)
{
    if (!arg)
        return !optHeadless && g_file_test(startPagePath, G_FILE_TEST_EXISTS) ? g_filename_to_uri(startPagePath, nullptr, nullptr)
                                                                             : g_strdup("about:blank");
    if (!strstr(arg, "://") && g_file_test(arg, G_FILE_TEST_EXISTS)) {
        GFile* file = g_file_new_for_commandline_arg(arg);
        char* uri = g_file_get_uri(file);
        g_object_unref(file);
        return uri;
    }
    char* uri = resolveInput(arg);
    return uri ? uri : g_strdup("about:blank");
}

static int uiMain(int argc, char** argv)
{
    GOptionContext* context = g_option_context_new("[URL|FILE|WORDS]");
    g_option_context_add_main_entries(context, optionEntries, nullptr);
    GError* error = nullptr;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        fprintf(stderr, "wpe-browser: %s\n", error->message);
        g_option_context_free(context);
        return 1;
    }
    g_option_context_free(context);
    optionsFromEnvironment();
    applyProcessModel(); /* also WPE_BROWSER_STALL_SECS, which the watchdog reads */
    snprintf(processRole, sizeof(processRole), "ui");
    initStallReport(-1);
    presentStallMs.store(optFrameStallSecs * 1000);
    if (optFrameStallSecs > 0)
        LOG("frame-watch present-stall-secs=%d stall-secs=%d", optFrameStallSecs, optStallSecs);
    startWatchdog(0);

    int width = 1024, height = 768;
    if (optSize && sscanf(optSize, "%dx%d", &width, &height) != 2) {
        fprintf(stderr, "wpe-browser: --size wants WxH\n");
        return 1;
    }
    if (optCPURendering)
        g_setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", TRUE); /* inherited by the WebProcess */
    if (optRSSSecs > 0) {
        g_autofree char* secs = g_strdup_printf("%d", optRSSSecs);
        g_setenv("WPE_BROWSER_RSS_SECS", secs, TRUE); /* the children's watchdog logs theirs */
    }

    /* home (Alt+Home): the start page; without one (headless) the first page */
    char* argURI = optURIs && optURIs[0] ? initialURI(optURIs[0]) : nullptr;
    homeURI = initialURI(nullptr);
    if (argURI && !strcmp(homeURI, "about:blank")) {
        g_free(homeURI);
        homeURI = g_strdup(argURI);
    }
    if (optCycle)
        loadCycle(optCycle);
    const char* firstURI = argURI ? argURI
        : cyclePages && cyclePages->len ? static_cast<const char*>(g_ptr_array_index(cyclePages, cycleNextIndex++)) : homeURI;

    LOG("start pid=%d webkit=%u.%u.%u mode=%s uri=%s exe=%s", static_cast<int>(getpid()), webkit_get_major_version(),
        webkit_get_minor_version(), webkit_get_micro_version(), optHeadless ? "headless" : "window", firstURI,
        getenv("WPE_PHOENIX_EXECUTABLE"));

    mainLoop = g_main_loop_new(nullptr, FALSE);

    startupPhase("display");
    WPEDisplay* display = nullptr;
    if (optHeadless) {
#if ENABLE_WPE_PLATFORM_HEADLESS
        display = wpe_display_headless_new();
#endif
    } else {
#if ENABLE_WPE_PLATFORM_WAYLAND
        display = wpe_display_wayland_new();
#endif
    }
    if (!display) {
        LOG("no WPE display for mode %s in this build", optHeadless ? "headless" : "window");
        return 1;
    }
    if (!wpe_display_connect(display, &error)) {
        LOG("display-connect-failed %s (WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s)", error ? error->message : "?",
            g_getenv("WAYLAND_DISPLAY"), g_getenv("XDG_RUNTIME_DIR"));
        g_clear_error(&error);
        return 1;
    }
    LOG("display %s", G_OBJECT_TYPE_NAME(display));
    g_signal_connect(display, "disconnected", G_CALLBACK(displayDisconnected), nullptr);
    /* before the first web process starts: the UI process reads WPE_PHOENIX_DMABUF then */
    const char* transport = chooseFrameTransport(display);
    const char* cpuRaster = g_getenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING"); /* WebKit's own test */
    /* Frame pacing (WebKit patch 0019, read by the UI process's backing store of the view): by
     * default the web process composites the next frame only once the compositor's frame callback
     * for this one came back, so the whole path runs in series once per frame; ahead, the two
     * sides overlap and the slower one sets the rate. */
    if (optFrameAhead)
        g_setenv("WPE_PHOENIX_FRAME_AHEAD", "1", TRUE);
    const char* frameAhead = g_getenv("WPE_PHOENIX_FRAME_AHEAD"); /* also WebKit's own knob */
    LOG("gpu raster=%s transport=%s webgl=%s frame-ahead=%d", cpuRaster && strcmp(cpuRaster, "0") ? "cpu" : "gpu", transport,
        !ENABLE_WEBGL ? (optWebGL ? "unbuilt" : "off") : optWebGL ? "on" : "off", frameAhead && !strcmp(frameAhead, "1") ? 1 : 0);

    startupPhase("network-session");
    WebKitNetworkSession* session = createNetworkSession();
    if (optIgnoreTLSErrors)
        webkit_network_session_set_tls_errors_policy(session, WEBKIT_TLS_ERRORS_POLICY_IGNORE);
    startupPhase("web-context");
    WebKitWebContext* webContext = createWebContext();
    startupPhase("web-view");
#if ENABLE_VIDEO
    /* <video>/<audio>: a build with media (the port's USE video: ENABLE_VIDEO with WebKit patch
     * 0030's FFmpeg player). A build without it has no engine, so media stay off there. */
    const char* autoplay = optAutoplay ? optAutoplay : "muted";
    WebKitAutoplayPolicy autoplayPolicy = WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND;
    if (!strcmp(autoplay, "allow"))
        autoplayPolicy = WEBKIT_AUTOPLAY_ALLOW;
    else if (!strcmp(autoplay, "deny"))
        autoplayPolicy = WEBKIT_AUTOPLAY_DENY;
    else if (strcmp(autoplay, "muted")) {
        fprintf(stderr, "wpe-browser: --autoplay wants muted, allow or deny\n");
        return 1;
    }
    LOG("media autoplay=%s", autoplay);
#endif
#if ENABLE_MEDIA_SOURCE
    /* Media Source Extensions (WebKit patch 0032's FFmpeg MSE engine). ManagedMediaSource, on by
     * default in WPE, stays off unless asked for: hls.js prefers it when it exists, and one MSE
     * surface is enough to start with (coordination repo docs/browser/MSE-DESIGN.md §5). */
    const char* mse = optMSE ? optMSE : "on";
    if (strcmp(mse, "on") && strcmp(mse, "managed") && strcmp(mse, "off")) {
        fprintf(stderr, "wpe-browser: --mse wants on, managed or off\n");
        return 1;
    }
    mseMode = mse;
#endif
    WebKitSettings* settings = webkit_settings_new_with_settings(
        "enable-webgl", static_cast<gboolean>(ENABLE_WEBGL && optWebGL),
        "enable-media", ENABLE_VIDEO ? TRUE : FALSE,
        "enable-webaudio", FALSE,
        "enable-developer-extras", FALSE,
        "enable-page-cache", FALSE,
        "enable-2d-canvas-acceleration", FALSE,
        "enable-write-console-messages-to-stdout", TRUE,
        nullptr);
    enableCSSFeatures(settings);
#if ENABLE_MEDIA_SOURCE
    webkit_settings_set_enable_mediasource(settings, strcmp(mse, "off") ? TRUE : FALSE);
    {
        WebKitFeatureList* features = webkit_settings_get_all_features();
        WebKitFeature* managed = webkit_feature_list_find(features, "ManagedMediaSource");
        if (managed)
            webkit_settings_set_feature_enabled(settings, managed, !strcmp(mse, "managed"));
        LOG("media mse=%s managed=%s", mse, managed ? (!strcmp(mse, "managed") ? "1" : "0") : "absent");
        webkit_feature_list_unref(features);
    }
#endif

    /* the chrome overlay, in its own script world (window mode) */
    WebKitUserContentManager* contentManager = webkit_user_content_manager_new();
    const char* toolbar = optHeadless || optNoChrome ? "never" : optToolbar ? optToolbar : "always";
    if (strcmp(toolbar, "always") && strcmp(toolbar, "auto") && strcmp(toolbar, "never")) {
        fprintf(stderr, "wpe-browser: --toolbar wants always, auto or never\n");
        return 1;
    }
    chrome.enabled = strcmp(toolbar, "never");
    chrome.pinned = !strcmp(toolbar, "always");
    LOG("chrome mode=%s", toolbar);
    if (chrome.pinned) {
        /* The toolbar is an overlay in the page (one WPE view has no room beside the page), so
         * the page makes room: its root box starts below the bar. A user style sheet's
         * !important wins over the page's own. Elements the page fixes at top: 0 stay under the
         * bar; documents the chrome script does not reach (SVG, XML, error pages) have neither. */
        WebKitUserStyleSheet* sheet = webkit_user_style_sheet_new(
            "html { margin-top: 34px !important; scroll-padding-top: 34px !important; }",
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_STYLE_LEVEL_USER, nullptr, nullptr);
        webkit_user_content_manager_add_style_sheet(contentManager, sheet);
        webkit_user_style_sheet_unref(sheet);
    }
    if (chrome.enabled) {
        WebKitUserScript* script = webkit_user_script_new_for_world(chromeScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, chromeWorld, nullptr, nullptr);
        webkit_user_content_manager_add_script(contentManager, script);
        webkit_user_script_unref(script);
        g_signal_connect(contentManager, "script-message-received::chrome", G_CALLBACK(chromeMessage), nullptr);
        if (!webkit_user_content_manager_register_script_message_handler(contentManager, "chrome", chromeWorld))
            LOG("chrome-error the script message handler is not registered");
        LOG("chrome on world=%s search=%s home=%s", chromeWorld, optSearch ? optSearch : defaultSearch, homeURI);
    }
#if ENABLE_MEDIA_SOURCE
    {
        WebKitUserScript* script = webkit_user_script_new_for_world(
            "window.webkit.messageHandlers.mediaCheck.postMessage(typeof MediaSource + ' ' + typeof ManagedMediaSource);",
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, mediaCheckWorld, nullptr, nullptr);
        webkit_user_content_manager_add_script(contentManager, script);
        webkit_user_script_unref(script);
        g_signal_connect(contentManager, "script-message-received::mediaCheck", G_CALLBACK(mediaCheckMessage), nullptr);
        if (!webkit_user_content_manager_register_script_message_handler(contentManager, "mediaCheck", mediaCheckWorld))
            LOG("media mse-check-error the script message handler is not registered");
    }
#endif

#if ENABLE_VIDEO
    WebKitWebsitePolicies* policies = webkit_website_policies_new_with_policies("autoplay", autoplayPolicy, nullptr);
#endif
    webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "display", display,
        "web-context", webContext,
        "network-session", session,
        "settings", settings,
        "user-content-manager", contentManager,
#if ENABLE_VIDEO
        "website-policies", policies,
#endif
        nullptr));
    g_object_unref(settings);
    g_object_unref(contentManager);
#if ENABLE_VIDEO
    g_object_unref(policies);
#endif

    g_signal_connect(webView, "load-changed", G_CALLBACK(loadChanged), nullptr);
    g_signal_connect(webView, "load-failed", G_CALLBACK(loadFailed), nullptr);
    g_signal_connect(webView, "load-failed-with-tls-errors", G_CALLBACK(loadFailedTLS), nullptr);
    g_signal_connect(webView, "web-process-terminated", G_CALLBACK(webProcessTerminated), nullptr);
    g_signal_connect(webView, "notify::page-id", G_CALLBACK(pageIDChanged), nullptr);
    g_signal_connect(webView, "notify::is-web-process-responsive", G_CALLBACK(responsiveChanged), nullptr);
    g_signal_connect(webView, "notify::title", G_CALLBACK(titleChanged), nullptr);
    g_signal_connect(webView, "notify::estimated-load-progress", G_CALLBACK(progressChanged), nullptr);
    g_signal_connect(webView, "decide-policy", G_CALLBACK(decidePolicy), nullptr);
    g_signal_connect(webView, "create", G_CALLBACK(createView), nullptr);

    if (WPEView* view = webkit_web_view_get_wpe_view(webView)) {
        g_signal_connect(view, "event", G_CALLBACK(viewEvent), nullptr);
        if (WPEToplevel* toplevel = wpe_view_get_toplevel(view)) {
            wpe_toplevel_resize(toplevel, width, height);
            wpe_toplevel_set_title(toplevel, "wpe-browser");
        }
        LOG("view %s %dx%d", G_OBJECT_TYPE_NAME(view), width, height);
        g_signal_connect(view, "buffer-rendered", G_CALLBACK(viewBufferRendered), nullptr);
        if (optPresentSecs > 0) {
            present.since = nowMs();
            g_signal_connect(view, "buffer-rendered", G_CALLBACK(presentBufferRendered), nullptr);
            g_timeout_add_seconds(static_cast<guint>(optPresentSecs), presentReport, nullptr);
        }
    }

    if (optTimeout > 0)
        g_timeout_add_seconds(optTimeout, loadTimeout, nullptr);
    if (cyclePages && cyclePages->len)
        g_timeout_add_seconds(static_cast<guint>(optCycleSecs), cycleNext, nullptr);
    if (optRSSSecs > 0) {
        logFootprint("ui");
        logSystemMemory();
        g_timeout_add_seconds(static_cast<guint>(optRSSSecs), logUIFootprint, nullptr);
    }
    if (optAuto)
        scheduleAuto(optAuto);
    g_unix_signal_add(SIGINT, quitOnSignal, nullptr);
    g_unix_signal_add(SIGTERM, quitOnSignal, nullptr);

    lastPageID = webkit_web_view_get_page_id(webView);
    startupPhase("first-load"); /* launches the first WebProcess */
    loadAddress(firstURI);
    startupPhase("main-loop"); /* until its first beat */
    g_main_loop_run(mainLoop);

    LOG("exit status=%d", exitStatus);
    g_object_unref(webView);
    g_object_unref(session);
    g_object_unref(display);
    g_main_loop_unref(mainLoop);
    return exitStatus;
}

int main(int argc, char** argv)
{
    startMs = nowMs();
    mainStackTop = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    setvbuf(stderr, nullptr, _IOLBF, 0);
    recordExecutablePath(argv[0]);

    /* Every role may open TLS connections through GIO (the NetworkProcess certainly does). */
    g_io_openssl_load(nullptr);

    const char* role = getenv("WPE_PHOENIX_PROCESS_ROLE");
    if (role && *role) {
        snprintf(processRole, sizeof(processRole), "%s", role);
        unsetenv("WPE_PHOENIX_PROCESS_ROLE"); /* not for this process's own children */
        LOG("role=%s pid=%d ppid=%d argc=%d", processRole, static_cast<int>(getpid()), static_cast<int>(getppid()), argc);
        if (strcmp(processRole, "web") && strcmp(processRole, "network")) {
            LOG("unknown role %s", processRole);
            return 1;
        }
        initStallReport(argc > 2 ? atoi(argv[2]) : -1);
        startWatchdog(getppid());
        startupPhase("process-main");
        int status = !strcmp(processRole, "web") ? WebKit::WebProcessMain(argc, argv) : WebKit::NetworkProcessMain(argc, argv);
        /* how long WebKit's own exit takes after the UI connection closed is the B4 orphan
         * question: this line and the watchdog's say which path ended the process */
        LOG("role=%s pid=%d main returned %d", processRole, static_cast<int>(getpid()), status);
        childExitStatus.store(status);
        exitStartMs.store(static_cast<int64_t>(nowMs())); /* the watchdog ends a hung exit() */
        return status;
    }
    return uiMain(argc, argv);
}
