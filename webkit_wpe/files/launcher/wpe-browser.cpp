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
 *     --web-extensions=DIR  the WebProcess loads the web process extensions (*.so) in DIR,
 *                           with the user data string "wpe-browser"
 *     --ephemeral           keep nothing on disk (the default with --headless)
 *     --data-dir=DIR        cookies, local storage, HSTS (default $HOME/.local/share/wpe-browser)
 *     --cache-dir=DIR       the HTTP disk cache (default $HOME/.cache/wpe-browser)
 *     --no-chrome           no toolbar overlay (the default with --headless)
 *     --search=PREFIX       where plain words go (default DuckDuckGo's HTML search)
 *   Test knobs (also from the environment, for runs started where only one argument fits):
 *     --cycle=LIST          load the next page of LIST every --cycle-secs (WPE_BROWSER_CYCLE):
 *                           comma-separated entries, or the path of a file with one per line
 *     --cycle-secs=S        default 60 (WPE_BROWSER_CYCLE_SECS)
 *     --rss-secs=S          every process logs its memory footprint every S s (WPE_BROWSER_RSS_SECS)
 *     --auto=STEPS          synthetic keyboard input (WPE_BROWSER_AUTO): comma-separated
 *                           <seconds>:key:<keys> (e.g. ctrl+l, alt+Left, F5, Return) or
 *                           <seconds>:type:<text>, fed through the same path as real keys
 *
 * The chrome (window mode) is an overlay the program injects into every top-level page, in its
 * own script world "wpe-browser": a toolbar (back, forward, reload/stop, home, the address) that
 * appears with Ctrl+L or when the pointer touches the top edge, and a progress line while a page
 * loads. The address is edited here, in the UI process; the overlay only displays it, so a page
 * never sees those keystrokes.
 *   Keys: Ctrl+L / Alt+D / F6 the address (Enter go, Escape cancel; plain words are a search),
 *   Alt+Left / Alt+Right back / forward, Ctrl+R / F5 reload, Ctrl+Shift+R reload without the
 *   cache, Escape stop, Alt+Home the start page, F11 fullscreen, Ctrl+Q quit.
 * Every line this program prints starts with "WPEB " (UART-friendly, one event per line);
 * every chrome action prints "WPEB t=<ms> chrome action=<what> source=key|ui|auto|cycle ...".
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cmakeconfig.h"

#include <algorithm>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
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

#define LOG(...)                                                    \
    do {                                                            \
        fprintf(stderr, "WPEB t=%.0f ", nowMs() - startMs);         \
        fprintf(stderr, __VA_ARGS__);                               \
        fputc('\n', stderr);                                        \
    } while (0)

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
 * A child never outlives the UI process. WebKit's children exit when they see their IPC
 * connection to the UI close, and back that up with 10 s watchdogs (the WebProcess's ends in
 * g_error(), which on Phoenix raises SIGTRAP rather than calling abort(), as GLib finds no
 * /proc/self/status). This is the Phoenix counterpart of Linux's PR_SET_PDEATHSIG: once the UI
 * is gone (the child has been reparented) the child gets a short grace period for WebKit's own
 * orderly exit, then _exit()s - so the next browser run never meets the previous run's children.
 * The same thread logs the child's memory footprint every WPE_BROWSER_RSS_SECS seconds.
 */
static constexpr unsigned orphanPollMs = 100;
static constexpr unsigned orphanGraceMs = 1500;

static char childRole[16];

static void* parentWatchdog(void* arg)
{
    const pid_t parent = static_cast<pid_t>(reinterpret_cast<intptr_t>(arg));
    const char* rss = getenv("WPE_BROWSER_RSS_SECS");
    const unsigned rssPolls = rss ? static_cast<unsigned>(atoi(rss)) * (1000 / orphanPollMs) : 0;
    unsigned polls = 0;
    while (getppid() == parent) {
        usleep(orphanPollMs * 1000);
        if (rssPolls && ++polls % rssPolls == 0)
            logFootprint(childRole);
    }
    usleep(orphanGraceMs * 1000);
    LOG("role=%s pid=%d orphaned (UI pid %d gone %u ms ago), exiting", childRole, static_cast<int>(getpid()),
        static_cast<int>(parent), orphanGraceMs);
    _exit(0);
    return nullptr;
}

static void startParentWatchdog()
{
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if (pthread_create(&thread, &attr, parentWatchdog, reinterpret_cast<void*>(static_cast<intptr_t>(getppid()))))
        LOG("role=%s pid=%d: no parent watchdog (pthread_create failed)", childRole, static_cast<int>(getpid()));
    pthread_attr_destroy(&attr);
}

/* --- UI role: options ----------------------------------------------------------------------- */

static gboolean optHeadless;
static char* optSnapshot;
static char* optSize;
static int optTimeout;
static gboolean optExitAfterLoad;
static gboolean optIgnoreTLSErrors;
static gboolean optCPURendering;
static char* optWebExtensions;
static gboolean optEphemeral;
static char* optDataDir;
static char* optCacheDir;
static gboolean optNoChrome;
static char* optSearch;
static char* optCycle;
static int optCycleSecs;
static int optRSSSecs;
static char* optAuto;
static char** optURIs;

static const GOptionEntry optionEntries[] = {
    { "headless", 0, 0, G_OPTION_ARG_NONE, &optHeadless, "No window (WPEPlatform headless display)", nullptr },
    { "snapshot", 0, 0, G_OPTION_ARG_FILENAME, &optSnapshot, "Save a PNG snapshot after the first load and exit", "FILE" },
    { "size", 0, 0, G_OPTION_ARG_STRING, &optSize, "View size", "WxH" },
    { "timeout", 0, 0, G_OPTION_ARG_INT, &optTimeout, "Fail (exit 2) if the first load takes longer", "S" },
    { "exit-after-load", 0, 0, G_OPTION_ARG_NONE, &optExitAfterLoad, "Exit when the first load has finished", nullptr },
    { "ignore-tls-errors", 0, 0, G_OPTION_ARG_NONE, &optIgnoreTLSErrors, "Accept invalid TLS certificates", nullptr },
    { "cpu-rendering", 0, 0, G_OPTION_ARG_NONE, &optCPURendering, "Skia CPU raster in the WebProcess", nullptr },
    { "web-extensions", 0, 0, G_OPTION_ARG_FILENAME, &optWebExtensions, "Web process extensions directory", "DIR" },
    { "ephemeral", 0, 0, G_OPTION_ARG_NONE, &optEphemeral, "Keep no cookies, cache or site data on disk", nullptr },
    { "data-dir", 0, 0, G_OPTION_ARG_FILENAME, &optDataDir, "Cookies and site data (default $HOME/.local/share/wpe-browser)", "DIR" },
    { "cache-dir", 0, 0, G_OPTION_ARG_FILENAME, &optCacheDir, "HTTP disk cache (default $HOME/.cache/wpe-browser)", "DIR" },
    { "no-chrome", 0, 0, G_OPTION_ARG_NONE, &optNoChrome, "No toolbar overlay", nullptr },
    { "search", 0, 0, G_OPTION_ARG_STRING, &optSearch, "Search URL prefix for plain words", "PREFIX" },
    { "cycle", 0, 0, G_OPTION_ARG_STRING, &optCycle, "Pages to cycle through (comma list or file)", "LIST" },
    { "cycle-secs", 0, 0, G_OPTION_ARG_INT, &optCycleSecs, "Seconds per cycled page (default 60)", "S" },
    { "rss-secs", 0, 0, G_OPTION_ARG_INT, &optRSSSecs, "Log every process's memory footprint every S s", "S" },
    { "auto", 0, 0, G_OPTION_ARG_STRING, &optAuto, "Synthetic key input: <s>:key:<keys>,<s>:type:<text>,...", "STEPS" },
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
        .progress.shown { display: block; }`;
    let host = null, ui = null, hover = false;
    let state = { editing: false, before: '', after: '', sel: false, uri: '', loading: false,
                  progress: 0, back: false, fwd: false };
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
        ui.bar.classList.toggle('shown', state.editing || hover);
        ui.progress.classList.toggle('shown', state.loading);
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
    g_string_append_printf(js, "\"editing\":%s,\"sel\":%s,\"loading\":%s,\"progress\":%.2f,\"back\":%s,\"fwd\":%s})",
        chrome.editing ? "true" : "false", chrome.selectAll ? "true" : "false",
        webkit_web_view_is_loading(webView) ? "true" : "false", webkit_web_view_get_estimated_load_progress(webView),
        webkit_web_view_can_go_back(webView) ? "true" : "false", webkit_web_view_can_go_forward(webView) ? "true" : "false");
    webkit_web_view_evaluate_javascript(webView, js->str, static_cast<gssize>(js->len), chromeWorld, nullptr, nullptr, nullptr, nullptr);
    g_string_free(js, TRUE);
}

static void loadAddress(const char* uri)
{
    if (uri && *uri)
        webkit_web_view_load_uri(webView, uri);
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
        if (ok)
            webkit_web_view_go_back(webView);
    } else if (!strcmp(action, "forward")) {
        bool ok = webkit_web_view_can_go_forward(webView);
        LOG("chrome action=forward source=%s ok=%d", source, ok);
        if (ok)
            webkit_web_view_go_forward(webView);
    } else if (!strcmp(action, "reload")) {
        LOG("chrome action=reload source=%s uri=%s", source, webkit_web_view_get_uri(webView));
        webkit_web_view_reload(webView);
    } else if (!strcmp(action, "reload-nocache")) {
        LOG("chrome action=reload-nocache source=%s uri=%s", source, webkit_web_view_get_uri(webView));
        webkit_web_view_reload_bypass_cache(webView);
    } else if (!strcmp(action, "stop")) {
        LOG("chrome action=stop source=%s loading=%d", source, webkit_web_view_is_loading(webView));
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
    LOG("load-failed uri=%s error=%s", uri, error ? error->message : "?");
    return FALSE; /* WebKit's error page */
}

static gboolean loadFailedTLS(WebKitWebView*, const char* uri, GTlsCertificate*, GTlsCertificateFlags errors, gpointer)
{
    LOG("load-failed-tls uri=%s flags=0x%x", uri, static_cast<unsigned>(errors));
    return FALSE;
}

static void webProcessTerminated(WebKitWebView*, WebKitWebProcessTerminationReason reason, gpointer)
{
    LOG("web-process-terminated reason=%s", reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed"
        : reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "memory-limit" : "api");
    if (optSnapshot || optExitAfterLoad)
        quit(3);
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

/* There is one view: target=_blank links and window.open() load in it. */
static gboolean decidePolicy(WebKitWebView* view, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer)
{
    if (type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
        return FALSE;
    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    LOG("new-window uri=%s opened=same-view via=policy", uri ? uri : "");
    webkit_policy_decision_ignore(decision);
    if (uri && *uri && strcmp(uri, "about:blank"))
        webkit_web_view_load_uri(view, uri);
    return TRUE;
}

static WebKitWebView* createView(WebKitWebView* view, WebKitNavigationAction* action, gpointer)
{
    const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    LOG("new-window uri=%s opened=same-view via=create", uri ? uri : "");
    if (uri && *uri && strcmp(uri, "about:blank"))
        webkit_web_view_load_uri(view, uri);
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
    LOG("cycle n=%u uri=%s", ++cycleNextIndex, uri);
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

static gboolean logUIFootprint(gpointer)
{
    logFootprint("ui");
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
        LOG("session ephemeral");
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
    WebKitNetworkSession* session = webkit_network_session_new(dataDir, cacheDir);
    g_autofree char* cookies = g_build_filename(dataDir, "cookies.sqlite", nullptr);
    WebKitCookieManager* cookieManager = webkit_network_session_get_cookie_manager(session);
    webkit_cookie_manager_set_persistent_storage(cookieManager, cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    webkit_cookie_manager_set_accept_policy(cookieManager, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
    /* the default, but the disk cache depends on it */
    webkit_web_context_set_cache_model(webkit_web_context_get_default(), WEBKIT_CACHE_MODEL_WEB_BROWSER);
    LOG("session persistent data=%s cache=%s cookies=%s cookie-policy=no-third-party cache-model=web-browser",
        dataDir, cacheDir, cookies);
    return session;
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

    /* The WebProcess's injected bundle (libWPEInjectedBundle.so, dlopen()ed) loads these; set
     * before the first web process starts. */
    if (optWebExtensions) {
        WebKitWebContext* webContext = webkit_web_context_get_default();
        webkit_web_context_set_web_process_extensions_directory(webContext, optWebExtensions);
        webkit_web_context_set_web_process_extensions_initialization_user_data(webContext, g_variant_new_string("wpe-browser"));
        LOG("web-extensions dir=%s", optWebExtensions);
    }

    WebKitNetworkSession* session = createNetworkSession();
    if (optIgnoreTLSErrors)
        webkit_network_session_set_tls_errors_policy(session, WEBKIT_TLS_ERRORS_POLICY_IGNORE);
    WebKitSettings* settings = webkit_settings_new_with_settings(
        "enable-webgl", FALSE,
        "enable-media", FALSE,
        "enable-webaudio", FALSE,
        "enable-developer-extras", FALSE,
        "enable-page-cache", FALSE,
        "enable-2d-canvas-acceleration", FALSE,
        "enable-write-console-messages-to-stdout", TRUE,
        nullptr);

    /* the chrome overlay, in its own script world (window mode) */
    WebKitUserContentManager* contentManager = webkit_user_content_manager_new();
    chrome.enabled = !optHeadless && !optNoChrome;
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

    webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "display", display,
        "network-session", session,
        "settings", settings,
        "user-content-manager", contentManager,
        nullptr));
    g_object_unref(settings);
    g_object_unref(contentManager);

    g_signal_connect(webView, "load-changed", G_CALLBACK(loadChanged), nullptr);
    g_signal_connect(webView, "load-failed", G_CALLBACK(loadFailed), nullptr);
    g_signal_connect(webView, "load-failed-with-tls-errors", G_CALLBACK(loadFailedTLS), nullptr);
    g_signal_connect(webView, "web-process-terminated", G_CALLBACK(webProcessTerminated), nullptr);
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
    }

    if (optTimeout > 0)
        g_timeout_add_seconds(optTimeout, loadTimeout, nullptr);
    if (cyclePages && cyclePages->len)
        g_timeout_add_seconds(static_cast<guint>(optCycleSecs), cycleNext, nullptr);
    if (optRSSSecs > 0) {
        logFootprint("ui");
        g_timeout_add_seconds(static_cast<guint>(optRSSSecs), logUIFootprint, nullptr);
    }
    if (optAuto)
        scheduleAuto(optAuto);
    g_unix_signal_add(SIGINT, quitOnSignal, nullptr);
    g_unix_signal_add(SIGTERM, quitOnSignal, nullptr);

    webkit_web_view_load_uri(webView, firstURI);
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
    setvbuf(stderr, nullptr, _IOLBF, 0);
    recordExecutablePath(argv[0]);

    /* Every role may open TLS connections through GIO (the NetworkProcess certainly does). */
    g_io_openssl_load(nullptr);

    const char* role = getenv("WPE_PHOENIX_PROCESS_ROLE");
    if (role && *role) {
        snprintf(childRole, sizeof(childRole), "%s", role);
        unsetenv("WPE_PHOENIX_PROCESS_ROLE"); /* not for this process's own children */
        LOG("role=%s pid=%d ppid=%d argc=%d", childRole, static_cast<int>(getpid()), static_cast<int>(getppid()), argc);
        if (strcmp(childRole, "web") && strcmp(childRole, "network")) {
            LOG("unknown role %s", childRole);
            return 1;
        }
        startParentWatchdog();
        int status = !strcmp(childRole, "web") ? WebKit::WebProcessMain(argc, argv) : WebKit::NetworkProcessMain(argc, argv);
        /* how long WebKit's own exit takes after the UI connection closed is the B4 orphan
         * question: this line and the watchdog's say which path ended the process */
        LOG("role=%s pid=%d main returned %d", childRole, static_cast<int>(getpid()), status);
        return status;
    }
    return uiMain(argc, argv);
}
