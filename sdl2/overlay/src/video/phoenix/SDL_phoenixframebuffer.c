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
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_PHOENIX

/*
 * Software (non-GL) framebuffer path. Games that create a plain SDL_Surface
 * window (no SDL_WINDOW_OPENGL) render into a CPU shadow buffer that
 * UpdateWindowFramebuffer copies to /dev/fb0. The in-scope GL applications
 * all use the GL path above; this exists so SDL_GetWindowSurface-style clients
 * are not left without a driver.
 */

#include "../SDL_sysvideo.h"

#include "SDL_phoenixvideo.h"
#include "SDL_phoenixframebuffer.h"

#include <unistd.h>

/* The fb0 byte order on the Pi 4 is little-endian ARGB (BGRA in memory). Exact
 * channel order for this CPU path is a Pi-test detail; ARGB8888 is the natural
 * pick and is corrected there if needed. */
#define PHOENIX_FB_FORMAT SDL_PIXELFORMAT_ARGB8888

int PHOENIX_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
    PHOENIX_VideoData *data = (PHOENIX_VideoData *)_this->driverdata;
    size_t need = (size_t)window->w * window->h * 4;

    if (data->fb_shadow_len < need) {
        void *nb = SDL_realloc(data->fb_shadow, need);
        if (!nb) {
            return SDL_OutOfMemory();
        }
        data->fb_shadow = nb;
        data->fb_shadow_len = need;
    }
    SDL_memset(data->fb_shadow, 0, need);

    *format = PHOENIX_FB_FORMAT;
    *pixels = data->fb_shadow;
    *pitch = window->w * 4;
    return 0;
}

int PHOENIX_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    PHOENIX_VideoData *data = (PHOENIX_VideoData *)_this->driverdata;
    size_t bytes = (size_t)window->w * window->h * 4;

    (void)rects;
    (void)numrects;

    if (data->fb_fd < 0 || !data->fb_shadow) {
        return 0;
    }
    /* Full-frame blit (the shadow pitch == fb pitch for a native-size window). */
    (void)lseek(data->fb_fd, 0, SEEK_SET);
    (void)write(data->fb_fd, data->fb_shadow, bytes);
    return 0;
}

void PHOENIX_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    PHOENIX_VideoData *data = (PHOENIX_VideoData *)_this->driverdata;
    (void)window;

    if (data->fb_shadow) {
        SDL_free(data->fb_shadow);
        data->fb_shadow = NULL;
        data->fb_shadow_len = 0;
    }
}

#endif /* SDL_VIDEO_DRIVER_PHOENIX */

/* vi: set ts=4 sw=4 expandtab: */
