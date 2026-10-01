/*
 * soup-smoke: the B0 target check of the webkit_deps port
 *
 * Exercises, in one static program, what WebKit takes from this port:
 *   - glib-networking's OpenSSL backend registered statically (g_io_openssl_load(NULL)),
 *   - libsoup 3 GETs over HTTPS (TLS version, cipher, certificate verdict against the
 *     system CA bundle) and HTTP/2 (ALPN h2 through nghttp2), content decoding,
 *   - libpsl's built-in public suffix list,
 *   - brotli, libwebp (+ demux/mux), woff2 and libxml2/libxslt, on data made here.
 * Every result is one line starting with a tag (SOUPCHK, PSLCHK, BROTLICHK, WEBPCHK,
 * WOFF2CHK, XSLTCHK), and the last line is "SOUPCHK summary pass=N fail=M".
 *
 * usage: soup-smoke [url ...]   (default: https://example.com/ https://nghttp2.org/)
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libsoup/soup.h>
#include <libpsl.h>
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <libxml/parser.h>
#include <libxslt/transform.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/xsltutils.h>

/* glib-networking's module entry point, called directly in a static program */
extern void g_io_openssl_load(GIOModule *module);

/* woff2-roundtrip.cc */
extern int woff2_roundtrip(const char *path, size_t *ttfLen, size_t *woff2Len, size_t *outLen);


static int npass, nfail, ngets, nh2, nbr;


static void verdict(int ok)
{
	if (ok) {
		npass++;
	}
	else {
		nfail++;
	}
}


static const char *tlsVersionName(GTlsProtocolVersion v)
{
	switch (v) {
		case G_TLS_PROTOCOL_VERSION_TLS_1_0: return "TLSv1.0";
		case G_TLS_PROTOCOL_VERSION_TLS_1_1: return "TLSv1.1";
		case G_TLS_PROTOCOL_VERSION_TLS_1_2: return "TLSv1.2";
		case G_TLS_PROTOCOL_VERSION_TLS_1_3: return "TLSv1.3";
		case G_TLS_PROTOCOL_VERSION_UNKNOWN: return "none";
		default: return "other";
	}
}


static const char *httpVersionName(SoupHTTPVersion v)
{
	switch (v) {
		case SOUP_HTTP_1_0: return "1.0";
		case SOUP_HTTP_1_1: return "1.1";
		case SOUP_HTTP_2_0: return "2";
		default: return "?";
	}
}


typedef struct {
	GMainLoop *loop;
	GBytes *body;
	GError *error;
} request_t;


static void onRead(GObject *source, GAsyncResult *res, gpointer data)
{
	request_t *r = data;

	r->body = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &r->error);
	g_main_loop_quit(r->loop);
}


static void checkUrl(SoupSession *session, const char *url)
{
	SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url);
	request_t r = { 0 };
	gint64 t0;
	const char *enc;
	int ok;

	if (msg == NULL) {
		printf("SOUPCHK GET %s FAIL bad-uri\n", url);
		verdict(0);
		return;
	}
	r.loop = g_main_loop_new(NULL, FALSE);
	t0 = g_get_monotonic_time();
	soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT, NULL, onRead, &r);
	g_main_loop_run(r.loop);

	if (r.error != NULL) {
		printf("SOUPCHK GET %s FAIL error=\"%s\" ms=%" G_GINT64_FORMAT "\n", url, r.error->message,
			(g_get_monotonic_time() - t0) / 1000);
		g_error_free(r.error);
		verdict(0);
	}
	else {
		enc = soup_message_headers_get_one(soup_message_get_response_headers(msg), "Content-Encoding");
		ok = (soup_message_get_status(msg) == SOUP_STATUS_OK);
		if (g_str_has_prefix(url, "https:")) {
			ok = ok && (soup_message_get_tls_peer_certificate_errors(msg) == 0) &&
				(soup_message_get_tls_protocol_version(msg) != G_TLS_PROTOCOL_VERSION_UNKNOWN);
		}
		printf("SOUPCHK GET %s %s status=%u http=%s tls=%s cipher=%s certerr=0x%x bytes=%" G_GSIZE_FORMAT
			" enc=%s ms=%" G_GINT64_FORMAT "\n",
			url, ok ? "ok" : "FAIL", soup_message_get_status(msg), httpVersionName(soup_message_get_http_version(msg)),
			tlsVersionName(soup_message_get_tls_protocol_version(msg)),
			soup_message_get_tls_ciphersuite_name(msg) ? soup_message_get_tls_ciphersuite_name(msg) : "none",
			(unsigned int)soup_message_get_tls_peer_certificate_errors(msg), g_bytes_get_size(r.body),
			enc ? enc : "identity", (g_get_monotonic_time() - t0) / 1000);
		g_bytes_unref(r.body);
		ngets++;
		if (ok && soup_message_get_http_version(msg) == SOUP_HTTP_2_0) {
			nh2++;
		}
		if (ok && enc != NULL && strcmp(enc, "br") == 0) {
			nbr++;
		}
		verdict(ok);
	}
	g_main_loop_unref(r.loop);
	g_object_unref(msg);
}


static void checkPsl(void)
{
	const psl_ctx_t *psl = psl_builtin();
	const char *reg = (psl != NULL) ? psl_registrable_domain(psl, "www.bbc.co.uk") : NULL;
	int ok = (psl != NULL) && (reg != NULL) && (strcmp(reg, "bbc.co.uk") == 0) && psl_is_public_suffix(psl, "co.uk") &&
		!psl_is_public_suffix(psl, "bbc.co.uk");

	printf("PSLCHK %s builtin=%s rules=%d www.bbc.co.uk->%s libpsl=%s\n", ok ? "ok" : "FAIL",
		psl_builtin_file_time() ? "yes" : "no", (psl != NULL) ? psl_suffix_count(psl) : -1, reg ? reg : "(null)",
		psl_get_version());
	verdict(ok);
}


static void checkBrotli(void)
{
	static uint8_t in[64 * 1024], comp[80 * 1024], out[64 * 1024];
	size_t compLen = sizeof(comp), outLen = sizeof(out), i;
	int ok;

	for (i = 0; i < sizeof(in); i++) {
		in[i] = (uint8_t)("phoenix-rtos webkit "[i % 20] ^ (i >> 10));
	}
	ok = BrotliEncoderCompress(5, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, sizeof(in), in, &compLen, comp) &&
		(BrotliDecoderDecompress(compLen, comp, &outLen, out) == BROTLI_DECODER_RESULT_SUCCESS) &&
		(outLen == sizeof(in)) && (memcmp(in, out, sizeof(in)) == 0);
	printf("BROTLICHK %s in=%zu comp=%zu version=0x%x\n", ok ? "ok" : "FAIL", sizeof(in), compLen,
		(unsigned int)BrotliDecoderVersion());
	verdict(ok);
}


static void checkWebp(void)
{
	enum { W = 16, H = 16 };
	uint8_t rgba[W * H * 4], *enc = NULL, *dec = NULL;
	size_t encLen;
	int w = 0, h = 0, frames = -1, ok, x, y;
	WebPData data, assembled = { NULL, 0 };
	WebPDemuxer *demux;
	WebPMux *mux;

	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			uint8_t *p = &rgba[(y * W + x) * 4];
			p[0] = (uint8_t)(x * 16);
			p[1] = (uint8_t)(y * 16);
			p[2] = (uint8_t)((x ^ y) * 16);
			p[3] = 255;
		}
	}
	encLen = WebPEncodeLosslessRGBA(rgba, W, H, W * 4, &enc);
	if (encLen != 0) {
		dec = WebPDecodeRGBA(enc, encLen, &w, &h);
	}
	ok = (encLen != 0) && (dec != NULL) && (w == W) && (h == H) && (memcmp(dec, rgba, sizeof(rgba)) == 0);

	/* the container API WebKit's image decoder uses (demux), and mux (assembling) */
	data.bytes = enc;
	data.size = encLen;
	demux = ok ? WebPDemux(&data) : NULL;
	if (demux != NULL) {
		frames = (int)WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
		WebPDemuxDelete(demux);
	}
	mux = ok ? WebPMuxCreate(&data, 0) : NULL;
	ok = ok && (frames == 1) && (mux != NULL) && (WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK) && (assembled.size != 0);
	printf("WEBPCHK %s lossless %dx%d enc=%zu decode=%s frames=%d mux=%zu version=0x%x\n", ok ? "ok" : "FAIL", W, H,
		encLen, (dec != NULL && memcmp(dec, rgba, sizeof(rgba)) == 0) ? "identical" : "differs", frames, assembled.size,
		(unsigned int)WebPGetDecoderVersion());
	WebPDataClear(&assembled);
	WebPMuxDelete(mux);
	WebPFree(dec);
	WebPFree(enc);
	verdict(ok);
}


static void checkWoff2(void)
{
	static const char *const fonts[] = {
		"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	};
	size_t ttfLen = 0, woff2Len = 0, outLen = 0, i;
	int rc = -1;

	for (i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
		rc = woff2_roundtrip(fonts[i], &ttfLen, &woff2Len, &outLen);
		if (rc != -1) {
			printf("WOFF2CHK %s %s ttf=%zu woff2=%zu decoded=%zu\n", (rc == 0) ? "ok" : "FAIL", fonts[i], ttfLen,
				woff2Len, outLen);
			verdict(rc == 0);
			return;
		}
	}
	printf("WOFF2CHK skip: no TrueType font found\n");
}


static void checkXslt(void)
{
	static const char xml[] = "<items><i>b</i><i>a</i><i>c</i></items>";
	static const char xsl[] =
		"<xsl:stylesheet version=\"1.0\" xmlns:xsl=\"http://www.w3.org/1999/XSL/Transform\">"
		"<xsl:output method=\"text\"/>"
		"<xsl:template match=\"/\"><xsl:for-each select=\"items/i\"><xsl:sort select=\".\"/>"
		"<xsl:value-of select=\".\"/></xsl:for-each></xsl:template></xsl:stylesheet>";
	xmlDocPtr doc = xmlReadMemory(xml, sizeof(xml) - 1, "in.xml", NULL, 0);
	xmlDocPtr sdoc = xmlReadMemory(xsl, sizeof(xsl) - 1, "in.xsl", NULL, 0);
	xsltStylesheetPtr style = (sdoc != NULL) ? xsltParseStylesheetDoc(sdoc) : NULL;
	xmlDocPtr res = (style != NULL && doc != NULL) ? xsltApplyStylesheet(style, doc, NULL) : NULL;
	xmlChar *text = NULL;
	int len = 0, ok;

	if (res != NULL) {
		(void)xsltSaveResultToString(&text, &len, res, style);
	}
	ok = (text != NULL) && (len == 3) && (memcmp(text, "abc", 3) == 0);
	printf("XSLTCHK %s sorted=\"%.*s\" libxml2=%s libxslt=%s\n", ok ? "ok" : "FAIL", text ? len : 0,
		text ? (const char *)text : "", LIBXML_DOTTED_VERSION, LIBXSLT_DOTTED_VERSION);
	xmlFree(text);
	xmlFreeDoc(res);
	if (style != NULL) {
		xsltFreeStylesheet(style); /* frees sdoc */
	}
	else {
		xmlFreeDoc(sdoc);
	}
	xmlFreeDoc(doc);
	verdict(ok);
}


int main(int argc, char *argv[])
{
	static const char *const defaults[] = { "https://example.com/", "https://nghttp2.org/" };
	GTlsBackend *backend;
	SoupSession *session;
	int i;

	setvbuf(stdout, NULL, _IOLBF, 0);

	/* the TLS backend of a static program: register it before ANY TLS use */
	g_io_openssl_load(NULL);
	backend = g_tls_backend_get_default();
	printf("SOUPCHK tls-backend %s %s\n", G_OBJECT_TYPE_NAME(backend), g_tls_backend_supports_tls(backend) ? "ok" : "FAIL");
	verdict(g_tls_backend_supports_tls(backend) && strcmp(G_OBJECT_TYPE_NAME(backend), "GTlsBackendOpenssl") == 0);

	checkPsl();
	checkBrotli();
	checkWebp();
	checkWoff2();
	checkXslt();

	session = soup_session_new_with_options("timeout", 30, "user-agent", "soup-smoke (Phoenix-RTOS)", NULL);
	printf("SOUPCHK libsoup %u.%u.%u glib %u.%u.%u\n", soup_get_major_version(), soup_get_minor_version(),
		soup_get_micro_version(), glib_major_version, glib_minor_version, glib_micro_version);
	if (argc > 1) {
		for (i = 1; i < argc; i++) {
			checkUrl(session, argv[i]);
		}
	}
	else {
		for (i = 0; i < (int)(sizeof(defaults) / sizeof(defaults[0])); i++) {
			checkUrl(session, defaults[i]);
		}
	}
	g_object_unref(session);

	/* HTTP/2 is negotiated by ALPN; nghttp2.org (a default URL) always answers h2 */
	printf("SOUPCHK http2 %s h2=%d of %d GET(s) ok; Content-Encoding br decoded in %d\n", (nh2 > 0) ? "ok" : "FAIL", nh2,
		ngets, nbr);
	verdict(nh2 > 0);

	printf("SOUPCHK summary pass=%d fail=%d\n", npass, nfail);
	return (nfail == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
