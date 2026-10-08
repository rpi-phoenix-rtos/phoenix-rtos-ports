/*
 * webkit-browser: the desktop web browser of Phoenix-RTOS (WebKitGTK 2.54, GTK 3, Wayland;
 * coordination repo docs/browser/B10-WEBKITGTK.md).
 *
 * One static multi-call program, as wpe-browser (the WPE build): Phoenix has no shared libraries,
 * so the UI process, the WebProcess and the NetworkProcess are the same ELF. WebKit's
 * ProcessLauncher (shared patch 0007) execs WPE_PHOENIX_EXECUTABLE -- this program, recorded
 * below -- with the child's role in WPE_PHOENIX_PROCESS_ROLE and WebKit's usual argv.
 *
 * UI role: WebKit's MiniBrowser/gtk window (tabs, downloads bar, find bar, zoom, settings) with
 * the desktop defaults below.
 *   webkit-browser [options] [URL|FILE|WORDS...]     each argument opens in its own tab
 *     --private             keep nothing on disk (an ephemeral session)
 *     --data-dir=DIR        cookies, local storage, HSTS (default $HOME/.local/share/webkit-browser)
 *     --cache-dir=DIR       the HTTP disk cache (default $HOME/.cache/webkit-browser)
 *     --download-dir=DIR    where downloads go (default $HOME/Downloads)
 *     --cpu-rendering       no GPU: CPU raster, frames through shared memory, no WebGL
 *                           (hardware-acceleration-policy NEVER)
 *     --desktop-gl          let GDK ask for desktop OpenGL (by default GDK_GL=gles: our Mesa's
 *                           wayland variant is GLES-only, and GDK 3 binds EGL_OPENGL_API unless told)
 *     --no-webgl            WebGL off (on by default in a build with the port's USE flag webgl)
 *     --autoplay=POLICY     allow | muted (default) | deny, for <video>/<audio>
 *     --no-mse              Media Source Extensions off (in a build with USE mse)
 *     --search=PREFIX       where plain words go (default DuckDuckGo's HTML search)
 *     --process-cache=N     web processes kept for recently left sites (default 2, 0 none;
 *                           patch 0015: WPE_PHOENIX_PROCESS_CACHE)
 *     --prewarm             keep a spare web process launched ahead (WPE_PHOENIX_PREWARM)
 *     --no-process-swap     one web process for every site (WPE_PHOENIX_PROCESS_SWAP=0)
 *     --exit-after-load     quit once the first tab has loaded (checks)
 *     --timeout=S           exit with status 2 if the first tab has not loaded after S s (checks)
 *     --download=URL        once the first tab has loaded, download URL through it (checks: the
 *                           downloads bar, ~/Downloads, the "download finished" line)
 *     --tab-cycle=S         switch to the next tab every S s (checks: "tab switch ..." lines)
 *     --present-stats=S     every S s, how many frames GDK painted the window ("present-stats
 *                           paints=N fps=F"; the gate's painted fps of a <video>)
 *   Keys (MiniBrowser's): Ctrl+T new tab, Ctrl+W close, Ctrl+L the address, F5 / Ctrl+R reload,
 *   Escape stop, Alt+Home start page, Ctrl+F find, Ctrl++ / Ctrl+- / Ctrl+0 zoom, F11 fullscreen,
 *   Ctrl+Q quit; Alt+Left / Alt+Right back / forward.
 *
 * Every line this program prints starts with "WKGB " (UART-friendly, one event per line):
 * "WKGB t=<ms> ui start ...", "load finished/failed ...", "download started/finished/failed ...",
 * "web-process-terminated ...", and "role=web|network pid=..." from the children. The web
 * process's own "WPEB-WEBKIT swap-chain ... type=texture-dmabuf|shm" line (shared patch 0016)
 * says which frame transport it took; GTK's "Disabled hardware acceleration because GTK failed
 * to initialize GL" says GDK has no GL (then everything runs on the CPU). "WKGB gdk-gl ok|failed"
 * is this program's own check of GDK's GL on the browser window (see logGdkGL()).
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cmakeconfig.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <epoxy/egl.h>
#include <gdk/gdkwayland.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include "BrowserTab.h"
#include "BrowserWindow.h"

namespace WebKit {
int WebProcessMain(int argc, char** argv);
int NetworkProcessMain(int argc, char** argv);
}

/* glib-networking's OpenSSL TLS backend, linked statically (gio/modules/libgioopenssl.a) */
extern "C" void g_io_openssl_load(GIOModule*);

/* --- log ------------------------------------------------------------------------------------ */

/* bumped with every change a Pi gate depends on: printed in "ui start" */
static const char launcherRevision[] = "b10-r3";

static double startMs;
static char processRole[16] = "ui";

static double nowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

__attribute__((format(printf, 1, 2)))
static void logLine(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fprintf(stderr, "WKGB t=%.0f %s\n", nowMs() - startMs, line);
}
#define LOG(...) logLine(__VA_ARGS__)

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
    const char* path = getenv("PATH");
    char* copy = strdup(path ? path : "/bin:/usr/bin");
    char* save = nullptr;
    for (char* dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(nullptr, ":", &save)) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, argv0);
        if (!access(candidate, X_OK) && realpath(candidate, resolved)) {
            setenv("WPE_PHOENIX_EXECUTABLE", resolved, 1);
            break;
        }
    }
    free(copy);
}

/*
 * A child never outlives the UI process. WebKit's children exit when their IPC connection to the
 * UI closes; this is the backstop wpe-browser also has (Phoenix has no PR_SET_PDEATHSIG): once
 * the child has been reparented, a short grace period for WebKit's own orderly exit, then
 * _exit(), so the next browser run never meets the previous run's children.
 */
static constexpr unsigned orphanPollMs = 250;
static constexpr unsigned orphanGraceMs = 3000;

static void* orphanWatchdog(void* arg)
{
    const pid_t parent = static_cast<pid_t>(reinterpret_cast<intptr_t>(arg));
    while (getppid() == parent)
        usleep(orphanPollMs * 1000);
    usleep(orphanGraceMs * 1000);
    LOG("role=%s pid=%d orphaned (parent %d gone): exit", processRole, static_cast<int>(getpid()), static_cast<int>(parent));
    _exit(0);
    return nullptr;
}

static void startOrphanWatchdog(pid_t parent)
{
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if (pthread_create(&thread, &attr, orphanWatchdog, reinterpret_cast<void*>(static_cast<intptr_t>(parent))))
        LOG("role=%s pid=%d: no orphan watchdog (pthread_create failed)", processRole, static_cast<int>(getpid()));
    pthread_attr_destroy(&attr);
}

/* --- UI role: options ----------------------------------------------------------------------- */

static constexpr int defaultProcessCache = 2;

static gboolean optPrivate;
static char* optDataDir;
static char* optCacheDir;
static char* optDownloadDir;
static gboolean optCPURendering;
static gboolean optDesktopGL;
static gboolean optNoWebGL;
static char* optAutoplay;
static gboolean optNoMSE;
static char* optSearch;
static int optProcessCache = -1;
static gboolean optPrewarm;
static gboolean optNoProcessSwap;
static gboolean optExitAfterLoad;
static int optTimeout;
static char* optDownload;
static int optTabCycle;
static int optPresentStats;
static char** optURIs;

static const GOptionEntry optionEntries[] = {
    { "private", 0, 0, G_OPTION_ARG_NONE, &optPrivate, "Keep nothing on disk (ephemeral session)", nullptr },
    { "data-dir", 0, 0, G_OPTION_ARG_FILENAME, &optDataDir, "Cookies, local storage, HSTS", "DIR" },
    { "cache-dir", 0, 0, G_OPTION_ARG_FILENAME, &optCacheDir, "The HTTP disk cache", "DIR" },
    { "download-dir", 0, 0, G_OPTION_ARG_FILENAME, &optDownloadDir, "Where downloads go", "DIR" },
    { "cpu-rendering", 0, 0, G_OPTION_ARG_NONE, &optCPURendering, "No GPU: CPU raster, shared-memory frames, no WebGL", nullptr },
    { "desktop-gl", 0, 0, G_OPTION_ARG_NONE, &optDesktopGL, "Let GDK ask for desktop OpenGL instead of GLES", nullptr },
    { "no-webgl", 0, 0, G_OPTION_ARG_NONE, &optNoWebGL, "WebGL off", nullptr },
    { "autoplay", 0, 0, G_OPTION_ARG_STRING, &optAutoplay, "allow, muted (default) or deny", "POLICY" },
    { "no-mse", 0, 0, G_OPTION_ARG_NONE, &optNoMSE, "Media Source Extensions off", nullptr },
    { "search", 0, 0, G_OPTION_ARG_STRING, &optSearch, "Where plain words go (default DuckDuckGo)", "PREFIX" },
    { "process-cache", 0, 0, G_OPTION_ARG_INT, &optProcessCache, "Web processes kept for recently left sites (default 2)", "N" },
    { "prewarm", 0, 0, G_OPTION_ARG_NONE, &optPrewarm, "Keep a spare web process launched ahead", nullptr },
    { "no-process-swap", 0, 0, G_OPTION_ARG_NONE, &optNoProcessSwap, "One web process for every site", nullptr },
    { "exit-after-load", 0, 0, G_OPTION_ARG_NONE, &optExitAfterLoad, "Quit once the first tab has loaded", nullptr },
    { "timeout", 0, 0, G_OPTION_ARG_INT, &optTimeout, "Exit with status 2 if the first tab has not loaded after S s", "S" },
    { "download", 0, 0, G_OPTION_ARG_STRING, &optDownload, "Once the first tab has loaded, download URL (checks)", "URL" },
    { "tab-cycle", 0, 0, G_OPTION_ARG_INT, &optTabCycle, "Switch to the next tab every S s (checks)", "S" },
    { "present-stats", 0, 0, G_OPTION_ARG_INT, &optPresentStats, "Every S s, log how many frames the window painted", "S" },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &optURIs, nullptr, "[URL|FILE|WORDS...]" },
    { nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr }
};

/* --- UI role: what a typed or given text loads ----------------------------------------------- */

static const char* searchPrefix()
{
    return optSearch && *optSearch ? optSearch : "https://html.duckduckgo.com/html/?q=";
}

static bool looksLikeHost(const char* text)
{
    /* one word with a dot (example.org, 10.0.0.2:8080/x) or localhost[:port][/path] */
    if (strchr(text, ' '))
        return false;
    return strchr(text, '.') || !strncmp(text, "localhost", 9);
}

/*
 * The location entry's rule (MiniBrowser's BrowserWindow calls it through BROWSER_ENTRY_TO_URI,
 * patch webkit-gtk/0102): a URI with a scheme or about: as typed; an existing absolute path as
 * file://; a host name with https:// (http:// for localhost and IPv4 addresses); anything else
 * is a search.
 */
extern "C" char* phoenix_browser_entry_to_uri(const char* text)
{
    while (g_ascii_isspace(*text))
        ++text;
    g_autofree char* trimmed = g_strchomp(g_strdup(text));
    if (!*trimmed)
        return g_strdup("about:blank");
    const char* scheme = g_uri_peek_scheme(trimmed); /* interned, not to be freed */
    if ((scheme && strstr(trimmed, "://")) || g_str_has_prefix(trimmed, "about:") || g_str_has_prefix(trimmed, "javascript:")
        || g_str_has_prefix(trimmed, "data:"))
        return g_strdup(trimmed);
    if (trimmed[0] == '/' && g_file_test(trimmed, G_FILE_TEST_EXISTS))
        return g_filename_to_uri(trimmed, nullptr, nullptr);
    if (looksLikeHost(trimmed)) {
        bool plain = !strncmp(trimmed, "localhost", 9) || g_ascii_isdigit(trimmed[0]);
        return g_strconcat(plain ? "http://" : "https://", trimmed, nullptr);
    }
    g_autofree char* query = g_uri_escape_string(trimmed, nullptr, FALSE);
    return g_strconcat(searchPrefix(), query, nullptr);
}

/* a command-line argument: a file name relative to the current directory is a file */
static char* argumentToURI(const char* argument)
{
    if (argument[0] != '/' && !strstr(argument, "://") && g_file_test(argument, G_FILE_TEST_EXISTS)) {
        g_autoptr(GFile) file = g_file_new_for_commandline_arg(argument);
        return g_file_get_uri(file);
    }
    return phoenix_browser_entry_to_uri(argument);
}

/* --- UI role: downloads ---------------------------------------------------------------------- */

static char* downloadDirectory()
{
    if (optDownloadDir && *optDownloadDir)
        return g_strdup(optDownloadDir);
    return g_build_filename(g_get_home_dir(), "Downloads", nullptr);
}

/* ~/Downloads/<name>, or <name> (2), (3)... when it exists */
static gboolean downloadDecideDestination(WebKitDownload* download, const char* suggestedFilename, gpointer)
{
    g_autofree char* dir = downloadDirectory();
    if (g_mkdir_with_parents(dir, 0755))
        LOG("download mkdir %s: %s", dir, g_strerror(errno));
    g_autofree char* base = g_path_get_basename(suggestedFilename && *suggestedFilename ? suggestedFilename : "download");
    g_autofree char* path = g_build_filename(dir, base, nullptr);
    for (int n = 2; g_file_test(path, G_FILE_TEST_EXISTS) && n < 1000; ++n) {
        g_free(path);
        g_autofree char* numbered = g_strdup_printf("%s (%d)", base, n);
        path = g_build_filename(dir, numbered, nullptr);
    }
    /* the webkit2gtk-4.1 API takes a URI here (the 2022 API a path) */
    g_autofree char* uri = g_filename_to_uri(path, nullptr, nullptr);
    webkit_download_set_destination(download, uri);
    LOG("download destination %s", path);
    return TRUE;
}

static void downloadFinished(WebKitDownload* download, gpointer)
{
    WebKitURIResponse* response = webkit_download_get_response(download);
    LOG("download finished uri=%s destination=%s received=%" G_GUINT64_FORMAT " expected=%" G_GUINT64_FORMAT " secs=%.1f",
        response ? webkit_uri_response_get_uri(response) : "?", webkit_download_get_destination(download),
        webkit_download_get_received_data_length(download), response ? webkit_uri_response_get_content_length(response) : 0,
        webkit_download_get_elapsed_time(download));
}

static void downloadFailed(WebKitDownload* download, GError* error, gpointer)
{
    LOG("download failed destination=%s error=%s", webkit_download_get_destination(download), error ? error->message : "?");
}

static void downloadStarted(WebKitWebContext*, WebKitDownload* download, gpointer)
{
    WebKitURIRequest* request = webkit_download_get_request(download);
    LOG("download started uri=%s", request ? webkit_uri_request_get_uri(request) : "?");
    g_signal_connect(download, "decide-destination", G_CALLBACK(downloadDecideDestination), nullptr);
    g_signal_connect(download, "finished", G_CALLBACK(downloadFinished), nullptr);
    g_signal_connect(download, "failed", G_CALLBACK(downloadFailed), nullptr);
}

/* --- UI role: the window --------------------------------------------------------------------- */

static WebKitSettings* settings;
static WebKitUserContentManager* userContentManager;
static WebKitWebsitePolicies* websitePolicies;
static bool firstLoadDone;
static int exitStatus;

static void loadChanged(WebKitWebView* webView, WebKitLoadEvent event, gpointer first)
{
    if (event != WEBKIT_LOAD_FINISHED)
        return;
    LOG("load finished uri=%s title=%s", webkit_web_view_get_uri(webView), webkit_web_view_get_title(webView));
    if (first && !firstLoadDone) {
        firstLoadDone = true;
        if (optDownload && *optDownload) {
            g_autofree char* uri = phoenix_browser_entry_to_uri(optDownload);
            LOG("download request uri=%s", uri);
            /* (transfer full): the view keeps the reference */
            g_object_set_data_full(G_OBJECT(webView), "phoenix-check-download", webkit_web_view_download_uri(webView, uri), g_object_unref);
        }
        if (optExitAfterLoad)
            g_application_quit(g_application_get_default());
    }
}

static gboolean loadFailed(WebKitWebView*, WebKitLoadEvent, char* uri, GError* error, gpointer)
{
    LOG("load failed uri=%s error=%s", uri, error ? error->message : "?");
    return FALSE; /* WebKit's error page */
}

static void webProcessTerminated(WebKitWebView* webView, WebKitWebProcessTerminationReason reason, gpointer)
{
    LOG("web-process-terminated uri=%s reason=%s", webkit_web_view_get_uri(webView),
        reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed" : reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "memory-limit" : "api");
}

static WebKitWebView* createTab(BrowserWindow* window, bool first)
{
    auto* webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "web-context", browser_window_get_web_context(window),
        "settings", settings,
        "user-content-manager", userContentManager,
        "website-policies", websitePolicies,
        nullptr));
    g_signal_connect(webView, "load-changed", G_CALLBACK(loadChanged), first ? GINT_TO_POINTER(1) : nullptr);
    g_signal_connect(webView, "load-failed", G_CALLBACK(loadFailed), nullptr);
    g_signal_connect(webView, "web-process-terminated", G_CALLBACK(webProcessTerminated), nullptr);
    browser_window_append_view(window, webView);
    return webView;
}

static gboolean firstLoadTimeout(gpointer)
{
    if (!firstLoadDone) {
        LOG("timeout: the first tab has not loaded after %d s", optTimeout);
        exitStatus = 2;
        g_application_quit(g_application_get_default());
    }
    return G_SOURCE_REMOVE;
}

/*
 * GDK 3's way to the EGL display of a wl_display (gdk_wayland_get_display): the platform entry
 * points through eglGetProcAddress(). Not epoxy's eglGetPlatformDisplay(): epoxy resolves EGL 1.5
 * entry points only once a display is current (epoxy_conservative_egl_version() assumes 1.4
 * without one) and aborts with "No provider of eglGetPlatformDisplay found".
 */
static EGLDisplay getWaylandEGLDisplay(struct wl_display* wlDisplay)
{
    if (auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYPROC>(eglGetProcAddress("eglGetPlatformDisplay"))) {
        if (EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wlDisplay, nullptr))
            return display;
    }
    if (auto getPlatformDisplayEXT = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"))) {
        if (EGLDisplay display = getPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT, wlDisplay, nullptr))
            return display;
    }
    return eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(wlDisplay));
}

/*
 * The GL check of the gate (B10 G0): whether GDK gives this window an EGL context, as WebKit's
 * hardware acceleration needs (AcceleratedBackingStore::canUseHardwareAcceleration). One line:
 *   WKGB gdk-gl ok use_es=1 version=3.1
 *   WKGB gdk-gl failed error=<GDK's message>
 * and when it failed, the same steps as GDK's gdk_wayland_display_init_gl() on GDK's own
 * wl_display, each with its result and eglGetError(), to name the step:
 *   WKGB egl-probe platform_wayland=<0|1> display=<0|1> initialize=<0|1> version=<M.m>
 *        bind_es=<0|1> bind_gl=<0|1> create_context=<0|1> error=0x<hex> vendor=<...>
 */
static void probeEGL(GdkDisplay* display)
{
    struct wl_display* wlDisplay = gdk_wayland_display_get_wl_display(display);
    const char* clientExtensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    bool platformWayland = clientExtensions && (strstr(clientExtensions, "EGL_KHR_platform_wayland") || strstr(clientExtensions, "EGL_EXT_platform_wayland"));
    EGLDisplay eglDisplay = getWaylandEGLDisplay(wlDisplay);
    EGLint error = eglGetError();
    EGLint major = 0, minor = 0;
    bool initialized = false, bindES = false, bindGL = false, createContext = false;
    const char* vendor = "-";
    if (eglDisplay != EGL_NO_DISPLAY) {
        initialized = eglInitialize(eglDisplay, &major, &minor);
        error = eglGetError();
        if (initialized) {
            vendor = eglQueryString(eglDisplay, EGL_VENDOR);
            createContext = epoxy_has_egl_extension(eglDisplay, "EGL_KHR_create_context");
            bindGL = eglBindAPI(EGL_OPENGL_API);
            bindES = eglBindAPI(EGL_OPENGL_ES_API); /* last: GDK's GLES contexts expect this API */
        }
    }
    LOG("egl-probe platform_wayland=%d display=%d initialize=%d version=%d.%d bind_es=%d bind_gl=%d create_context=%d error=0x%x vendor=%s",
        platformWayland, eglDisplay != EGL_NO_DISPLAY, initialized, major, minor, bindES, bindGL, createContext, error, vendor ? vendor : "-");
}

/*
 * The same GL question asked before GTK and WebKit start, on a Wayland connection of its own:
 * every step GDK 3 and WebKit's AcceleratedBackingStore take, each with its result, so that a
 * "Disabled hardware acceleration because GTK failed to initialize GL" (which WebKit decides
 * while the web context is created, before any window) is explained by the line above it:
 *   WKGB egl-early wayland=1 client_ext=<platform_wayland,platform_surfaceless> display=1
 *        initialize=1 version=1.5 apis=<EGL_CLIENT_APIS> bind_es=1 bind_gl=0 create_context_ext=1
 *        configs=<n> context_es3=1 context_es2=1 error=0x3000 vendor=<...>
 * The connection stays open (closing it could let GDK's display reuse its address, which Mesa
 * keys its EGL displays on); the EGL display is terminated.
 */
static void earlyEGLProbe()
{
    struct wl_display* wlDisplay = wl_display_connect(nullptr);
    if (!wlDisplay) {
        LOG("egl-early wayland=0 (no compositor at WAYLAND_DISPLAY=%s)", g_getenv("WAYLAND_DISPLAY"));
        return;
    }
    const char* client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    bool platformWayland = client && (strstr(client, "EGL_KHR_platform_wayland") || strstr(client, "EGL_EXT_platform_wayland"));
    bool platformSurfaceless = client && strstr(client, "EGL_MESA_platform_surfaceless");
    EGLDisplay display = getWaylandEGLDisplay(wlDisplay);
    EGLint error = eglGetError();
    EGLint major = 0, minor = 0, configs = 0;
    bool initialized = false, bindES = false, bindGL = false, createContextExt = false, es3 = false, es2 = false;
    const char* vendor = "-";
    const char* apis = "-";
    if (display != EGL_NO_DISPLAY) {
        initialized = eglInitialize(display, &major, &minor);
        error = eglGetError();
        if (initialized) {
            vendor = eglQueryString(display, EGL_VENDOR);
            apis = eglQueryString(display, EGL_CLIENT_APIS);
            createContextExt = epoxy_has_egl_extension(display, "EGL_KHR_create_context");
            bindGL = eglBindAPI(EGL_OPENGL_API);
            bindES = eglBindAPI(EGL_OPENGL_ES_API);
            error = eglGetError();
            /* GDK 3's window config (find_eglconfig_for_window): RGB window surfaces, 8 bits */
            const EGLint configAttributes[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
                EGL_RED_SIZE, 1, EGL_GREEN_SIZE, 1, EGL_BLUE_SIZE, 1, EGL_ALPHA_SIZE, 8, EGL_NONE };
            EGLConfig config = nullptr;
            if (eglChooseConfig(display, configAttributes, &config, 1, &configs) && configs > 0) {
                const EGLint es3Attributes[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 0, EGL_NONE };
                const EGLint es2Attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
                EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, es3Attributes);
                es3 = context != EGL_NO_CONTEXT;
                if (es3)
                    eglDestroyContext(display, context);
                context = eglCreateContext(display, config, EGL_NO_CONTEXT, es2Attributes);
                es2 = context != EGL_NO_CONTEXT;
                if (es2)
                    eglDestroyContext(display, context);
            }
            error = eglGetError();
        }
    }
    LOG("egl-early wayland=1 client_ext=%s%s display=%d initialize=%d version=%d.%d apis=%s bind_es=%d bind_gl=%d create_context_ext=%d configs=%d context_es3=%d context_es2=%d error=0x%x vendor=%s",
        platformWayland ? "platform_wayland" : "-", platformSurfaceless ? ",platform_surfaceless" : "",
        display != EGL_NO_DISPLAY, initialized, major, minor, apis ? apis : "-", bindES, bindGL, createContextExt, configs, es3, es2,
        error, vendor ? vendor : "-");
    if (initialized)
        eglTerminate(display);
}

static void logGdkGL(GtkWidget* window)
{
    GError* error = nullptr;
    GdkGLContext* context = gdk_window_create_gl_context(gtk_widget_get_window(window), &error);
    if (context && gdk_gl_context_realize(context, &error)) {
        int major = 0, minor = 0;
        gdk_gl_context_get_version(context, &major, &minor);
        LOG("gdk-gl ok use_es=%d version=%d.%d", gdk_gl_context_get_use_es(context), major, minor);
    } else {
        LOG("gdk-gl failed error=%s", error ? error->message : "?");
        probeEGL(gtk_widget_get_display(window));
    }
    g_clear_error(&error);
    g_clear_object(&context);
}

/* --- UI role: checks ------------------------------------------------------------------------ */

static GtkWidget* findNotebook(GtkWidget* widget)
{
    if (GTK_IS_NOTEBOOK(widget))
        return widget;
    if (!GTK_IS_CONTAINER(widget))
        return nullptr;
    GtkWidget* found = nullptr;
    GList* children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList* l = children; l && !found; l = l->next)
        found = findNotebook(GTK_WIDGET(l->data));
    g_list_free(children);
    return found;
}

/* --tab-cycle: MiniBrowser's tabs are pages of the window's GtkNotebook */
static gboolean tabCycle(gpointer window)
{
    auto* notebook = GTK_NOTEBOOK(findNotebook(GTK_WIDGET(window)));
    if (!notebook)
        return G_SOURCE_CONTINUE;
    int pages = gtk_notebook_get_n_pages(notebook);
    if (pages < 2)
        return G_SOURCE_CONTINUE;
    int next = (gtk_notebook_get_current_page(notebook) + 1) % pages;
    gtk_notebook_set_current_page(notebook, next);
    GtkWidget* tab = gtk_notebook_get_nth_page(notebook, next);
    const char* uri = BROWSER_IS_TAB(tab) ? webkit_web_view_get_uri(browser_tab_get_web_view(BROWSER_TAB(tab))) : nullptr;
    LOG("tab switch page=%d/%d uri=%s", next + 1, pages, uri ? uri : "-");
    return G_SOURCE_CONTINUE;
}

/* --present-stats: GDK's frame clock paints the window once per frame that has damage */
static unsigned paintsSinceReport;
static double lastReportMs;

static void afterPaint(GdkFrameClock*, gpointer)
{
    ++paintsSinceReport;
}

static gboolean presentStats(gpointer)
{
    double now = nowMs();
    double secs = (now - lastReportMs) / 1000.0;
    LOG("present-stats secs=%.1f paints=%u fps=%.1f", secs, paintsSinceReport, secs > 0 ? paintsSinceReport / secs : 0.0);
    paintsSinceReport = 0;
    lastReportMs = now;
    return G_SOURCE_CONTINUE;
}

/* MiniBrowser's main.c sets the window's keys on the application; so do we */
static void startup(GApplication* application)
{
    static const char* actionAccels[] = {
        "win.reload", "F5", "<Ctrl>R", nullptr,
        "win.reload-no-cache", "<Ctrl>F5", "<Ctrl><Shift>R", nullptr,
        "win.focus-location", "<Ctrl>L", "<Alt>D", "F6", nullptr,
        "win.stop-load", "Escape", nullptr,
        "win.load-homepage", "<Alt>Home", nullptr,
        "win.go-back", "<Alt>Left", nullptr,
        "win.go-forward", "<Alt>Right", nullptr,
        "win.zoom-in", "<Ctrl>plus", "<Ctrl>equal", "<Ctrl>KP_Add", nullptr,
        "win.zoom-out", "<Ctrl>minus", "<Ctrl>KP_Subtract", nullptr,
        "win.zoom-default", "<Ctrl>0", "<Ctrl>KP_0", nullptr,
        "win.find", "<Ctrl>F", nullptr,
        "win.new-tab", "<Ctrl>T", nullptr,
        "win.toggle-fullscreen", "F11", nullptr,
        "win.close", "<Ctrl>W", nullptr,
        "win.quit", "<Ctrl>Q", nullptr,
        "find.next", "F3", "<Ctrl>G", nullptr,
        "find.previous", "<Shift>F3", "<Ctrl><Shift>G", nullptr,
        nullptr
    };
    for (const char** it = actionAccels; it[0]; it += g_strv_length(const_cast<char**>(it)) + 1)
        gtk_application_set_accels_for_action(GTK_APPLICATION(application), it[0], &it[1]);
}

static void activate(GApplication* application, gpointer)
{
    WebKitWebsiteDataManager* manager;
    if (optPrivate)
        manager = webkit_website_data_manager_new_ephemeral();
    else {
        g_autofree char* dataDir = optDataDir ? g_strdup(optDataDir) : g_build_filename(g_get_user_data_dir(), "webkit-browser", nullptr);
        g_autofree char* cacheDir = optCacheDir ? g_strdup(optCacheDir) : g_build_filename(g_get_user_cache_dir(), "webkit-browser", nullptr);
        manager = webkit_website_data_manager_new("base-data-directory", dataDir, "base-cache-directory", cacheDir, nullptr);
        LOG("session persistent data=%s cache=%s", dataDir, cacheDir);
    }
    webkit_website_data_manager_set_favicons_enabled(manager, TRUE);
    WebKitWebContext* context = webkit_web_context_new_with_website_data_manager(manager);
    g_object_unref(manager);

    WebKitCookieManager* cookies = webkit_web_context_get_cookie_manager(context);
    webkit_cookie_manager_set_accept_policy(cookies, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
    if (!optPrivate) {
        g_autofree char* cookieFile = g_build_filename(webkit_website_data_manager_get_base_data_directory(webkit_web_context_get_website_data_manager(context)),
            "cookies.sqlite", nullptr);
        webkit_cookie_manager_set_persistent_storage(cookies, cookieFile, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    }
    g_signal_connect(context, "download-started", G_CALLBACK(downloadStarted), nullptr);

    settings = webkit_settings_new();
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
    webkit_settings_set_enable_developer_extras(settings, FALSE);
    /* console.log() of the pages to stdout, as wpe-browser: the check pages report through it
     * ("B8PAGE ...", "B10 ...") */
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
    webkit_settings_set_hardware_acceleration_policy(settings,
        optCPURendering ? WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER : WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);
#if ENABLE_WEBGL
    webkit_settings_set_enable_webgl(settings, !optNoWebGL && !optCPURendering);
#endif
#if ENABLE_VIDEO
    webkit_settings_set_enable_media(settings, TRUE);
#endif
#if ENABLE_MEDIA_SOURCE
    webkit_settings_set_enable_mediasource(settings, !optNoMSE);
#endif
    userContentManager = webkit_user_content_manager_new();

    WebKitAutoplayPolicy autoplay = WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND;
    if (!g_strcmp0(optAutoplay, "allow"))
        autoplay = WEBKIT_AUTOPLAY_ALLOW;
    else if (!g_strcmp0(optAutoplay, "deny"))
        autoplay = WEBKIT_AUTOPLAY_DENY;
    websitePolicies = webkit_website_policies_new_with_policies("autoplay", autoplay, nullptr);

    auto* window = BROWSER_WINDOW(browser_window_new(nullptr, context));
    g_object_unref(context);
    gtk_application_add_window(GTK_APPLICATION(application), GTK_WINDOW(window));
    gtk_window_set_default_size(GTK_WINDOW(window), 1280, 960);

    GtkWidget* firstTab = nullptr;
    if (optURIs && optURIs[0]) {
        for (int i = 0; optURIs[i]; ++i) {
            WebKitWebView* webView = createTab(window, !i);
            if (!i)
                firstTab = GTK_WIDGET(webView);
            g_autofree char* uri = argumentToURI(optURIs[i]);
            LOG("open tab=%d uri=%s", i, uri);
            webkit_web_view_load_uri(webView, uri);
        }
    } else {
        WebKitWebView* webView = createTab(window, true);
        firstTab = GTK_WIDGET(webView);
        webkit_web_view_load_uri(webView, BROWSER_DEFAULT_URL);
    }
    if (optTimeout > 0)
        g_timeout_add_seconds(optTimeout, firstLoadTimeout, nullptr);

    gtk_widget_grab_focus(firstTab);
    gtk_widget_show(GTK_WIDGET(window));
    logGdkGL(GTK_WIDGET(window));
    if (optTabCycle > 0)
        g_timeout_add_seconds(optTabCycle, tabCycle, window);
    if (optPresentStats > 0) {
        if (GdkFrameClock* clock = gtk_widget_get_frame_clock(GTK_WIDGET(window))) {
            g_signal_connect(clock, "after-paint", G_CALLBACK(afterPaint), nullptr);
            lastReportMs = nowMs();
            g_timeout_add_seconds(optPresentStats, presentStats, nullptr);
        }
    }
    LOG("ui window shown gdk_gl=%s rendering=%s", g_getenv("GDK_GL") ? g_getenv("GDK_GL") : "desktop", optCPURendering ? "cpu" : "gpu");
}

/* The process model of shared patch 0015, read when the web context is created. */
static void applyProcessModel()
{
    int cache = optProcessCache >= 0 ? optProcessCache : defaultProcessCache;
    g_autofree char* value = g_strdup_printf("%d", cache);
    g_setenv("WPE_PHOENIX_PROCESS_CACHE", value, TRUE);
    g_setenv("WPE_PHOENIX_PREWARM", optPrewarm ? "1" : "0", TRUE);
    g_setenv("WPE_PHOENIX_PROCESS_SWAP", optNoProcessSwap ? "0" : "1", TRUE);
}

/* Outside a session (psh, a serial console): the first Wayland socket of the runtime directory. */
static void findWaylandDisplay()
{
    if (!g_getenv("HOME"))
        g_setenv("HOME", "/root", TRUE);
    if (!g_getenv("XDG_RUNTIME_DIR"))
        g_setenv("XDG_RUNTIME_DIR", "/tmp/xdg", TRUE);
    if (g_getenv("WAYLAND_DISPLAY"))
        return;
    for (int n = 0; n < 10; ++n) {
        g_autofree char* name = g_strdup_printf("wayland-%d", n);
        g_autofree char* path = g_build_filename(g_getenv("XDG_RUNTIME_DIR"), name, nullptr);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            g_setenv("WAYLAND_DISPLAY", name, TRUE);
            return;
        }
    }
}

static bool argumentGiven(int argc, char** argv, const char* option)
{
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--"))
            break;
        if (!strcmp(argv[i], option))
            return true;
    }
    return false;
}

static int uiMain(int argc, char** argv)
{
    /*
     * Everything GDK reads from the environment is set BEFORE the options are parsed: GTK's option
     * group runs gdk_pre_parse() as its pre-parse hook, and that is where GDK reads GDK_GL, once
     * (build 65, B10 G0: GDK_GL set after the parse was never seen, GDK bound EGL_OPENGL_API on our
     * GLES-only Mesa, and WebKit fell back to CPU rendering with "No GL implementation is
     * available"). Host test: coordination repo tools/browser/webkitgtk/gdkgl-order.c.
     */
    findWaylandDisplay();
    /* GDK 3 binds EGL_OPENGL_API unless GDK_GL=gles; our Mesa (wayland variant) has GLES only */
    if (!argumentGiven(argc, argv, "--desktop-gl"))
        g_setenv("GDK_GL", "gles", FALSE);
    /* only Wayland exists here; say so before GDK looks for X11 or broadway */
    gdk_set_allowed_backends("wayland");

    g_autoptr(GOptionContext) options = g_option_context_new(nullptr);
    g_option_context_add_main_entries(options, optionEntries, nullptr);
    g_option_context_add_group(options, gtk_get_option_group(FALSE));
    g_autoptr(GError) error = nullptr;
    if (!g_option_context_parse(options, &argc, &argv, &error)) {
        fprintf(stderr, "webkit-browser: %s\n", error->message);
        return 1;
    }

    applyProcessModel();
    /* launcher=: which revision of this file the binary carries (build 66 shipped an older one) */
    LOG("ui start pid=%d executable=%s wayland=%s gdk_gl=%s launcher=%s", static_cast<int>(getpid()), getenv("WPE_PHOENIX_EXECUTABLE"),
        g_getenv("WAYLAND_DISPLAY"), g_getenv("GDK_GL") ? g_getenv("GDK_GL") : "desktop", launcherRevision);
    if (!optCPURendering)
        earlyEGLProbe();

    GtkApplication* application = gtk_application_new("org.phoenix_rtos.WebBrowser", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "startup", G_CALLBACK(startup), nullptr);
    g_signal_connect(application, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(application), 0, nullptr);
    g_object_unref(application);
    LOG("ui exit status=%d", exitStatus ? exitStatus : status);
    return exitStatus ? exitStatus : status;
}

int main(int argc, char** argv)
{
    startMs = nowMs();
    setvbuf(stderr, nullptr, _IOLBF, 0);
    recordExecutablePath(argv[0]);

    /* Every role may open TLS connections through GIO (the NetworkProcess certainly does). */
    g_io_openssl_load(nullptr);

    const char* role = getenv("WPE_PHOENIX_PROCESS_ROLE");
    if (role && *role) {
        snprintf(processRole, sizeof(processRole), "%s", role);
        unsetenv("WPE_PHOENIX_PROCESS_ROLE"); /* not for this process's own children */
        LOG("role=%s pid=%d ppid=%d", processRole, static_cast<int>(getpid()), static_cast<int>(getppid()));
        if (strcmp(processRole, "web") && strcmp(processRole, "network")) {
            LOG("unknown role %s", processRole);
            return 1;
        }
        startOrphanWatchdog(getppid());
        int status = !strcmp(processRole, "web") ? WebKit::WebProcessMain(argc, argv) : WebKit::NetworkProcessMain(argc, argv);
        LOG("role=%s pid=%d main returned %d", processRole, static_cast<int>(getpid()), status);
        return status;
    }
    return uiMain(argc, argv);
}
