/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * Phoenix-RTOS platform backend for QuakeSpasm (QuakeSpasm is Copyright (C) id
 * Software, Inc. and the QuakeSpasm developers, GPL-2.0-or-later). It implements
 * the QuakeSpasm platform interface and is distributed under the same license as
 * the program it is built into; see COPYING in this directory.
 */
/*
 * pl_phoenix_glctx.c — V3D/Mesa GL context + offscreen FBO for the Quakespasm
 * Phoenix port. Mesa-header-only (pipe/st), deliberately kept separate from Quake's
 * headers (pl_phoenix_vid.c) to avoid type collisions. Same recipe as rpi4-glcube:
 * surfaceless st_create_context + a renderbuffer-backed RGBA8 + DEPTH24 FBO that
 * Quake renders into; pl_phoenix_vid.c presents it to /dev/fb0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "main/menums.h"
#include "frontend/api.h"
#include "main/mtypes.h"
#include "state_tracker/st_context.h"
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include "GL/gl.h"
#include "GL/glext.h"

struct pipe_screen_config;
struct renderonly;
struct pipe_screen *v3d_screen_create(int fd, const struct pipe_screen_config *config,
                                      struct renderonly *ro);
extern unsigned char _mesa_make_current(struct gl_context *ctx,
                                        struct gl_framebuffer *drawFb,
                                        struct gl_framebuffer *readFb);

/* winsys (v3d_phoenix_winsys.c): one-shot request that the NEXT BO be backed by the scanout fb,
 * a query of whether the scanout surface was claimed, whether DOUBLE-BUFFER+page-flip is active,
 * and the page-flip itself (display buffer 0 or 1). */
extern void v3d_phoenix_set_next_scanout(void);
extern int  v3d_phoenix_scanout_active(void);
extern int  v3d_phoenix_scanout_double(void);
extern int  v3d_phoenix_scanout_nbuf(void);   /* page-flip buffer count: 1 (none), 2 (double), 3 (triple) */
extern void v3d_phoenix_flip(int buf);

static struct st_context *g_st = NULL;

/* RENDER DIRECTLY INTO THE BACK BUFFER + PAGE-FLIP — the textbook double/triple-buffered present.
 *
 * The intermittent render wedge came from the GPU storing the depth-tested frame straight into the
 * buffer the HVS display controller was *actively scanning out*: that GPU-write/display-read
 * contention stalls the V3D depth-output FIFO on heavy frames. With >=2 scanout buffers and a
 * page-flip, the buffer we render into is NEVER the one being displayed (with 3 buffers it is >=2
 * flips away), so the contention — and therefore the wedge — cannot occur, WITHOUT a separate DRAM
 * render target. An earlier design rendered to a DRAM RT then blit-resolved to the scanout fb; that
 * extra hop was redundant with triple-buffering AND introduced an R/B-swap bug: the large DRAM RT
 * tripped the same `scanout`-RT heuristic (=> one `swap_color_rb` during the geometry render), then
 * glBlitFramebuffer's shader fallback applied a SECOND swap on the scanout dst — two swaps cancel,
 * so the world rendered blue. Rendering straight into the scanout BO is exactly ONE swap (the proven-
 * correct direct path: brown), with no blit and no second swap. Each back buffer shares one depth BO
 * (cleared per frame, never scanned out). */
static GLuint g_scanout_fbo[3] = {0, 0, 0}; /* scanout-fb-backed color+depth FBO(s) — Quake renders here */
static GLuint g_render_fbo  = 0;     /* DRAM color+depth FBO — ONLY for the no-scanout CPU-present fallback */
static GLuint g_capture_fbo = 0;     /* normal (non-aliased) RGBA8 FBO: scanout is blitted here so glReadPixels
                                      * reads a real CPU-backed BO (screenshot capture; the scanout FBO's own
                                      * resource CPU-map is a separate unwritten alloc -> glReadPixels=noise) */
static int    g_resolve     = 0;     /* 1 = scanout fb claimed (direct-render+page-flip path active) */
static int    g_double      = 0;     /* 1 = page-flip available (>=2 scanout buffers) */
static int    g_nbuf        = 1;     /* page-flip buffer count (1/2/3) — triple closes the flip race */
static int    g_back        = 0;     /* which scanout buffer we render into this frame (round-robin) */
static int    g_w = 0, g_h = 0;

/* Bind the framebuffer Quake renders this frame into. Quake targets the "default" framebuffer (0),
 * incomplete on a surfaceless context; redirect it. In the scanout path that is the current BACK
 * buffer (g_back), which the page-flip in qsv3d_resolve() then puts on screen; otherwise the DRAM
 * fallback FBO that GL_EndRendering reads back and CPU-presents. */
void qsv3d_bind_fbo(void)
{
	if (g_resolve)
		glBindFramebuffer(GL_FRAMEBUFFER, g_scanout_fbo[g_double ? g_back : 0]);
	else if (g_render_fbo != 0)
		glBindFramebuffer(GL_FRAMEBUFFER, g_render_fbo);
}

/* Present the just-rendered back buffer by page-flipping the display to it, then advance to the
 * next back buffer for the following frame. Returns 1 if it presented (scanout path), 0 if not
 * active (CPU-present fallback handles it). The caller has already glFinish()ed, so the frame is
 * fully rendered before the flip latches at vsync. With 3 buffers the next render target is >=2
 * flips from being scanned out -> the GPU never writes the displayed buffer (no contention, no
 * wedge). Single-buffer scanout (g_double==0) renders in place and is already on screen. */
int qsv3d_resolve(void)
{
	if (!g_resolve)
		return 0;
	if (g_double) {
		v3d_phoenix_flip(g_back);            /* display the buffer we just rendered into (winsys tracks the pan) */
		g_back = (g_back + 1) % g_nbuf;      /* next frame renders into a non-displayed buffer */
	}
	return 1;
}

/* Screenshot capture for the deterministic demo-capture harness. glReadPixels on the
 * render-to-scanout FBO returns noise (its resource's CPU mapping is a fresh unwritten alloc;
 * the GPU stored to the aliased fb PA). So BLIT the currently-bound (just-rendered) scanout FBO
 * to a normal DRAM FBO on the GPU — which reads the scanout color via its GPU-VA correctly —
 * then glReadPixels the normal FBO, whose CPU mapping IS the GPU-written BO. Fills `pix` with
 * w*h*3 RGB bytes, bottom-to-top (GL convention, matches the TGA writer -> no Y-flip). Returns
 * 1 on success, 0 if unavailable (caller falls back to a plain glReadPixels). Called from
 * SCR_CaptureTick, pre-flip, with the render-target FBO still bound. */
int qsv3d_capture_gl(void *pix, int w, int h);
int qsv3d_capture_gl(void *pix, int w, int h)
{
	GLint prev = 0;
	GLuint src;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
	/* Use the EXPLICIT render FBO Quake drew this frame into — NOT the currently-bound one.
	 * By capture time the render target is often unbound back to FB0 (GL_EndRendering re-binds
	 * it for the same reason); reading FB0 was noise. */
	if (g_resolve)
		src = g_scanout_fbo[g_double ? g_back : 0];
	else if (g_render_fbo)
		src = g_render_fbo;
	else
		return 0;

	if (g_capture_fbo != 0) {
		/* STRAIGHT GPU blit of the scanout FBO -> a normal FBO (reads scanout color via GPU-VA
		 * correctly; then glReadPixels a real CPU-backed BO). Do NOT flip Y in the blit — the v3d
		 * gallium blit does not honor an inverted destination (it writes constant garbage); flip
		 * in software below. */
		glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_capture_fbo);
		glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, g_capture_fbo);
	}
	else {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
	}
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glFinish();
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pix);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);   /* restore render target */

	/* Software vertical flip: the FBO is Y_0_TOP but glReadPixels + the TGA writer are
	 * bottom-origin, so the readback is upside-down. Reverse rows -> upright. */
	{
		size_t stride = (size_t)w * 3;
		unsigned char *base = (unsigned char *)pix;
		for (int r = 0; r < h / 2; r++) {
			unsigned char *a = base + (size_t)r * stride;
			unsigned char *b = base + (size_t)(h - 1 - r) * stride;
			for (size_t i = 0; i < stride; i++) {
				unsigned char t = a[i]; a[i] = b[i]; b[i] = t;
			}
		}
	}
	return 1;
}

int qsv3d_init(int w, int h)
{
	struct pipe_screen_config cfg;
	struct pipe_screen *pscreen;
	struct pipe_context *pipe;
	struct gl_config visual;
	struct st_config_options opts;
	struct st_context *st;
	GLuint fbo = 0, rbColor = 0, rbDepth = 0, rbScanDepth = 0;
	GLenum fbs;

	memset(&cfg, 0, sizeof(cfg));
	pscreen = v3d_screen_create(0, &cfg, NULL);
	if (!pscreen) { printf("qsv3d: pipe_screen NULL\n"); return 1; }
	pipe = pscreen->context_create(pscreen, NULL, 0);
	if (!pipe) { printf("qsv3d: pipe_context NULL\n"); return 1; }

	memset(&visual, 0, sizeof(visual));
	memset(&opts, 0, sizeof(opts));
	st = st_create_context(API_OPENGL_COMPAT, pipe, &visual, NULL, &opts, 0, 0);
	if (!st) { printf("qsv3d: st_create_context NULL\n"); return 1; }
	_mesa_make_current(st->ctx, NULL, NULL);
	g_st = st;

	printf("qsv3d: GL up; %s / %s\n",
	       (const char *)glGetString(GL_VERSION),
	       (const char *)glGetString(GL_RENDERER));

	g_w = w; g_h = h;

	/* SCANOUT FBO(s): Quake renders DIRECTLY into these — each color renderbuffer claims a scanout
	 * buffer (set_next_scanout() -> the winsys hands buffer 0, 1, 2) and now also carries a DEPTH
	 * attachment so depth-tested 3D renders straight to the back buffer. One depth renderbuffer is
	 * SHARED across all buffers (rendering is serialized and depth is cleared each frame; never
	 * scanned out). In double/triple-buffer mode we make one FBO per fb buffer and page-flip
	 * between them; single-buffer mode makes one (rendered in place). */
	g_double = v3d_phoenix_scanout_double();
	g_nbuf = v3d_phoenix_scanout_nbuf();
	{
		int n = g_double ? g_nbuf : 1;
		glGenRenderbuffers(1, &rbScanDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, rbScanDepth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
		for (int i = 0; i < n; i++) {
			GLuint rbScan = 0;
			glGenFramebuffers(1, &g_scanout_fbo[i]);
			glBindFramebuffer(GL_FRAMEBUFFER, g_scanout_fbo[i]);
			glGenRenderbuffers(1, &rbScan);
			glBindRenderbuffer(GL_RENDERBUFFER, rbScan);
			v3d_phoenix_set_next_scanout();   /* the next RT alloc claims scanout buffer i */
			glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbScan);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbScanDepth);
		}
		g_resolve = v3d_phoenix_scanout_active();   /* did the fb get claimed? */
		fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		printf("qsv3d: scanout FBO(s) %dx%d n=%d resolve=%d double=%d status=0x%x (%s)\n",
		       w, h, n, g_resolve, g_double, fbs,
		       g_resolve ? (g_double ? "direct-render+page-flip" : "single direct-render")
		                 : "unavailable -> CPU-present fallback");
	}

	/* DRAM FALLBACK render FBO (color + depth): used ONLY when the scanout fb couldn't be claimed,
	 * so GL_EndRendering glReadPixels()es it and CPU-presents. In the normal scanout path Quake
	 * renders straight to the back buffers above and this is never bound. */
	if (!g_resolve) {
		glGenFramebuffers(1, &fbo);
		g_render_fbo = fbo;
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glGenRenderbuffers(1, &rbColor);
		glBindRenderbuffer(GL_RENDERBUFFER, rbColor);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbColor);
		glGenRenderbuffers(1, &rbDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, rbDepth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbDepth);
		fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		printf("qsv3d: DRAM fallback render FBO %dx%d status=0x%x (complete=0x%x)\n",
		       w, h, fbs, GL_FRAMEBUFFER_COMPLETE);
	}

	/* CAPTURE FBO: a normal (DRAM-backed, non-scanout) RGBA8 target. glReadPixels on a scanout
	 * FBO reads its resource's fresh CPU mapping (= noise, the GPU stored to the aliased fb PA);
	 * a normal FBO's CPU map IS the GPU-written BO, so we blit scanout->here then read here.
	 * Only needed in the scanout path (the DRAM-fallback g_render_fbo is already readable). */
	if (g_resolve) {
		GLuint rbCap = 0;
		glGenFramebuffers(1, &g_capture_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, g_capture_fbo);
		glGenRenderbuffers(1, &rbCap);
		glBindRenderbuffer(GL_RENDERBUFFER, rbCap);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbCap);
		printf("qsv3d: capture FBO %dx%d status=0x%x\n", w, h, glCheckFramebufferStatus(GL_FRAMEBUFFER));
	}

	glViewport(0, 0, w, h);

	/* Clear every buffer to black so an un-rendered frame shows black, not garbage. */
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	if (g_resolve) {
		for (int i = 0; i < (g_double ? g_nbuf : 1); i++) {
			glBindFramebuffer(GL_FRAMEBUFFER, g_scanout_fbo[i]);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, g_scanout_fbo[0]);
	}
	else {
		glBindFramebuffer(GL_FRAMEBUFFER, g_render_fbo);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}
	glFinish();
	return 0;
}

void qsv3d_make_current(void)
{
	if (g_st)
		_mesa_make_current(g_st->ctx, NULL, NULL);
}

/* Mesa's trace gallium wrapper (referenced by the GL state tracker) is not built
 * into libv3d-phoenix; we never enable GALLIUM_TRACE, so pass the context through. */
struct pipe_context *trace_context_create_threaded(struct pipe_screen *screen,
                                                    struct pipe_context *pipe)
{
	(void)screen;
	return pipe;
}
