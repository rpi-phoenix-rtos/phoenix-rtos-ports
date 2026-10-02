/*
 * phx-probe-extension.so: the web process extension of the injected-bundle Pi check.
 *
 * WebKit loads web process extensions from the injected bundle: the WebProcess dlopen()s
 * libWPEInjectedBundle.so and calls its WKBundleInitialize, which creates the process's
 * WebKitWebProcessExtension and loads every .so in the directory the UI process set
 * (wpe-browser --web-extensions=DIR). So this extension's two lines prove the whole chain on
 * Phoenix: the bundle was loaded and initialised, it loaded this object in turn, and the
 * WebKitWebPage wrappers the bundle creates for each page exist.
 *
 *   WPEB-EXT init extension=yes user-data=<the string the UI process passed>
 *   WPEB-EXT page-created id=<page id> pid=<web process>
 *
 * and, per document, which web process loaded it (the UI's "page-swap page-id=" lines name the
 * same page ids):
 *
 *   WPEB-EXT document-loaded id=<page id> pid=<web process> uri=<uri>
 *
 * Every symbol it uses is in launcher/wpe-browser.exports. It declares them itself rather than
 * including the WebKit and GLib headers: the prototypes below are those of WebKit 2.54 and
 * GLib 2.88, and the object is linked -nostdlib (see build.sh) so that it binds to the copies in
 * wpe-browser instead of carrying its own.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

typedef struct _WebKitWebProcessExtension WebKitWebProcessExtension;
typedef struct _WebKitWebPage WebKitWebPage;
typedef struct _GVariant GVariant;

extern void g_printerr(const char *format, ...);
extern const char *g_variant_get_string(GVariant *value, size_t *length);
extern unsigned long g_signal_connect_data(void *instance, const char *detailedSignal, void (*handler)(void),
	void *data, void (*destroyData)(void *data, void *closure), int connectFlags);
extern uint64_t webkit_web_page_get_id(WebKitWebPage *page);
extern const char *webkit_web_page_get_uri(WebKitWebPage *page);

/* the entry point WebProcessExtensionManager looks up */
void webkit_web_process_extension_initialize_with_user_data(WebKitWebProcessExtension *extension, GVariant *userData);


static void phx_probe_documentLoaded(WebKitWebPage *page, void *data)
{
	const char *uri = webkit_web_page_get_uri(page);

	(void)data;
	g_printerr("WPEB-EXT document-loaded id=%llu pid=%d uri=%s\n", (unsigned long long)webkit_web_page_get_id(page), (int)getpid(),
		(uri != NULL) ? uri : "");
}


static void phx_probe_pageCreated(WebKitWebProcessExtension *extension, WebKitWebPage *page, void *data)
{
	(void)extension;
	(void)data;
	g_printerr("WPEB-EXT page-created id=%llu pid=%d\n", (unsigned long long)webkit_web_page_get_id(page), (int)getpid());
	g_signal_connect_data(page, "document-loaded", (void (*)(void))phx_probe_documentLoaded, NULL, NULL, 0);
}


void webkit_web_process_extension_initialize_with_user_data(WebKitWebProcessExtension *extension, GVariant *userData)
{
	g_printerr("WPEB-EXT init extension=%s user-data=%s\n", (extension != NULL) ? "yes" : "no",
		(userData != NULL) ? g_variant_get_string(userData, NULL) : "(none)");
	g_signal_connect_data(extension, "page-created", (void (*)(void))phx_probe_pageCreated, NULL, NULL, 0);
}
