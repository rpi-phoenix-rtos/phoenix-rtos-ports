/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#ifndef SDL_phoenixvideo_h_
#define SDL_phoenixvideo_h_

#include "../../SDL_internal.h"
#include "../SDL_sysvideo.h"

#include <stdint.h>
#include <stddef.h>

/* Per-device state for the Phoenix-RTOS video driver. A single, always-
 * fullscreen window backed by /dev/fb0; the GL back end renders with the
 * in-process ported Mesa V3D stack straight into the scanout framebuffer. */
typedef struct
{
    int fb_fd;                /* /dev/fb0 (open for the whole session) */
    int tty_fd;               /* /dev/tty0|console, for the fbcon graphics-mode switch */
    unsigned int width;       /* native mode, from the RPI4FB_GETMODE ioctl */
    unsigned int height;
    unsigned int pitch;       /* bytes per scanline */
    uint32_t fb_pa;           /* framebuffer physical base address */
    uint64_t fb_len;          /* framebuffer size in bytes */
    int scanout;              /* 1 if the V3D winsys claimed the fb for render-to-scanout */
    SDL_Window *window;       /* the single fullscreen window (only one supported) */
    int gl_created;           /* 1 once the GL context has been created */
    void *fb_shadow;          /* CPU shadow buffer for the non-GL framebuffer path */
    size_t fb_shadow_len;
} PHOENIX_VideoData;

/*
 * GL-context bring-up seam.
 *
 * These entry points are the plain-C interface to the in-process Mesa V3D GL
 * context + scanout FBO. They are DELIBERATELY left unresolved in libSDL2.a:
 * their implementation needs Mesa-internal headers (pipe/*, state_tracker/*,
 * main/mtypes.h) plus a set of Mesa-specific compile flags that this SDL cross
 * build never provides. They are supplied by a small glue translation unit that
 * the *game* (or the SDL2 GL test) compiles with the Mesa flags and links
 * alongside libSDL2.a + libv3d-phoenix.a + libGL-phoenix.a. See the port's
 * glue/ directory. (Licensing: that glue TU is zlib-licensed and self-
 * contained; it is kept OUTSIDE libSDL2.a, which also stays zlib. A symbol
 * reference from the driver to the glue is not a derivative work.)
 *
 * phxgl_init         create the V3D screen + GL state-tracker context and the
 *                    scanout-backed FBO(s); makes the context current. Returns 0
 *                    on success.
 * phxgl_make_current re-bind the context to the calling thread.
 * phxgl_resolve      present the just-rendered back buffer (page-flip); returns
 *                    1 if it presented (scanout path), 0 if not active.
 * phxgl_bind_fbo     bind the framebuffer the next frame renders into (the
 *                    surfaceless context has no usable default framebuffer 0).
 */
extern int phxgl_init(int w, int h);
extern void phxgl_make_current(void);
extern int phxgl_resolve(void);
extern void phxgl_bind_fbo(void);

/*
 * V3D framebuffer winsys hooks. These live in libv3d-phoenix.a (not GPL); the
 * driver hands the scanout framebuffer's physical address + geometry to the
 * winsys at VideoInit so the GL context can render straight to screen.
 */
extern int v3d_phoenix_scanout_init(uint32_t pa, uint32_t w, uint32_t h, uint32_t pitch);
extern int v3d_phoenix_scanout_active(void);
extern void v3d_phoenix_flip(int buf);

#endif /* SDL_phoenixvideo_h_ */

/* vi: set ts=4 sw=4 expandtab: */
