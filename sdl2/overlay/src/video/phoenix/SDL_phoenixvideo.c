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
 * Phoenix-RTOS video driver (Raspberry Pi 4 / BCM2711).
 *
 * A single always-fullscreen window at the native /dev/fb0 mode. There is no
 * mode switching (VID_Restart is a no-op, as in the existing Phoenix game
 * ports). Rendering is done by the in-process ported Mesa V3D OpenGL stack
 * (see SDL_phoenixopengl.c); the scanout framebuffer's physical address and
 * geometry are handed to the V3D winsys here at init so the GL context can
 * render straight to screen. Input (keyboard + mouse) is drained from
 * /dev/kbd0 and /dev/mouse0 in PumpEvents (see SDL_phoenixevents.c).
 */

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"

#include "SDL_phoenixvideo.h"
#include "SDL_phoenixevents.h"
#include "SDL_phoenixopengl.h"
#include "SDL_phoenixframebuffer.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <phoenix/fbcon.h>

#define PHOENIXVID_DRIVER_NAME "phoenix"

/*
 * rpi4-fb client ABI (replicated here to avoid a phoenix-rtos-devices include
 * path). MUST match sources/phoenix-rtos-devices/video/rpi4-fb/rpi4-fb.h so the
 * _IOR command number agrees.
 */
typedef struct {
    uint16_t width, height, bpp, pitch;
    uint64_t smemlen;     /* framebuffer size in bytes (pitch*height) */
    uint64_t framebuffer; /* physical base address */
} rpi4fb_mode_t;
#define RPI4FB_GETMODE _IOR('g', 1, rpi4fb_mode_t)

/* Initialization/Query functions */
static int PHOENIX_VideoInit(_THIS);
static int PHOENIX_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode);
static void PHOENIX_VideoQuit(_THIS);
static int PHOENIX_CreateWindow(_THIS, SDL_Window *window);
static void PHOENIX_DestroyWindow(_THIS, SDL_Window *window);

/*
 * DOS-style graphics-mode switch: tell the HDMI text console (pl011-tty fbcon)
 * to stop drawing to the framebuffer while this fullscreen app owns it. Console
 * output is not lost (it accumulates in an off-screen shadow and reappears on
 * restore). This also causes pl011-tty to release /dev/kbd0 so the input driver
 * can open it. Without this the klog/psh mirror overdraws the rendered frame.
 */
static void PHOENIX_ConsoleSetMode(PHOENIX_VideoData *data, int mode)
{
    if (data->tty_fd < 0) {
        data->tty_fd = open("/dev/tty0", O_RDWR);
        if (data->tty_fd < 0) {
            data->tty_fd = open("/dev/console", O_RDWR);
        }
    }
    if (data->tty_fd >= 0) {
        (void)ioctl(data->tty_fd, FBCONSETMODE, mode);
    }
}

static void PHOENIX_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device->driverdata);
    SDL_free(device);
}

static SDL_VideoDevice *PHOENIX_CreateDevice(void)
{
    SDL_VideoDevice *device;
    PHOENIX_VideoData *data;

    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        return 0;
    }
    data = (PHOENIX_VideoData *)SDL_calloc(1, sizeof(PHOENIX_VideoData));
    if (!data) {
        SDL_free(device);
        SDL_OutOfMemory();
        return 0;
    }
    data->fb_fd = -1;
    data->tty_fd = -1;
    device->driverdata = data;

    /* General video */
    device->VideoInit = PHOENIX_VideoInit;
    device->VideoQuit = PHOENIX_VideoQuit;
    device->SetDisplayMode = PHOENIX_SetDisplayMode;
    device->PumpEvents = PHOENIX_PumpEvents;

    /* Window (single fullscreen window; no mode switching) */
    device->CreateSDLWindow = PHOENIX_CreateWindow;
    device->DestroyWindow = PHOENIX_DestroyWindow;

    /* Software framebuffer path (non-GL windows) */
    device->CreateWindowFramebuffer = PHOENIX_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = PHOENIX_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = PHOENIX_DestroyWindowFramebuffer;

    /* OpenGL over the in-process Mesa V3D stack (see SDL_phoenixopengl.c) */
    device->GL_LoadLibrary = PHOENIX_GL_LoadLibrary;
    device->GL_GetProcAddress = PHOENIX_GL_GetProcAddress;
    device->GL_UnloadLibrary = PHOENIX_GL_UnloadLibrary;
    device->GL_CreateContext = PHOENIX_GL_CreateContext;
    device->GL_MakeCurrent = PHOENIX_GL_MakeCurrent;
    device->GL_SetSwapInterval = PHOENIX_GL_SetSwapInterval;
    device->GL_GetSwapInterval = PHOENIX_GL_GetSwapInterval;
    device->GL_SwapWindow = PHOENIX_GL_SwapWindow;
    device->GL_DeleteContext = PHOENIX_GL_DeleteContext;

    device->free = PHOENIX_DeleteDevice;

    return device;
}

VideoBootStrap PHOENIX_bootstrap = {
    PHOENIXVID_DRIVER_NAME, "SDL Phoenix-RTOS fb0 + Mesa V3D video driver",
    PHOENIX_CreateDevice,
    NULL /* no ShowMessageBox implementation */
};

static int PHOENIX_VideoInit(_THIS)
{
    PHOENIX_VideoData *data = (PHOENIX_VideoData *)_this->driverdata;
    SDL_DisplayMode mode;
    rpi4fb_mode_t fbmode;

    /* Default native mode; overwritten by the real fb0 geometry if available. */
    data->width = 1920;
    data->height = 1080;
    data->pitch = data->width * 4;

    data->fb_fd = open("/dev/fb0", O_RDWR);
    if (data->fb_fd < 0) {
        /* No framebuffer: still register a mode so headless GL bring-up can be
         * link/smoke tested. Rendering will fall back to offscreen FBOs. */
        SDL_SetError("PHOENIX: /dev/fb0 open failed (continuing headless)");
    } else if (ioctl(data->fb_fd, RPI4FB_GETMODE, &fbmode) == 0 && fbmode.framebuffer != 0) {
        /* The V3D scanout winsys takes a 32-bit scanout PA. On the Pi 4 the fb0
         * scanout buffer always lives in low (<4 GiB) physical memory, so the
         * cast below never loses bits — but guard it explicitly rather than
         * truncate silently, so a future >4 GiB scanout PA can't corrupt the
         * value handed to the winsys (falls back to headless, like fb0 open). */
        if (fbmode.framebuffer > 0xffffffffULL) {
            SDL_SetError("PHOENIX: fb0 scanout PA %llu exceeds the 32-bit winsys limit (continuing headless)",
                (unsigned long long)fbmode.framebuffer);
        }
        else {
            data->width = fbmode.width;
            data->height = fbmode.height;
            data->pitch = fbmode.pitch;
            data->fb_pa = (uint32_t)fbmode.framebuffer;
            data->fb_len = fbmode.smemlen;
            /* Hand the scanout framebuffer to the V3D winsys BEFORE the GL context is
             * created (the fullscreen render target is allocated during context
             * create, so the winsys must already know the scanout PA to back it). */
            data->scanout = v3d_phoenix_scanout_init(data->fb_pa, data->width, data->height, data->pitch);
        }
    }

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB888;
    mode.w = (int)data->width;
    mode.h = (int)data->height;
    mode.refresh_rate = 60;
    mode.driverdata = NULL;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        return -1;
    }
    SDL_AddDisplayMode(&_this->displays[0], &mode);

    /* Claim the framebuffer: switch the HDMI text console to graphics mode. This
     * also prompts pl011-tty to release /dev/kbd0 for the input driver. */
    PHOENIX_ConsoleSetMode(data, FBCON_DISABLED);

    return 0;
}

static int PHOENIX_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    /* Single fixed native mode; nothing to switch. */
    (void)_this;
    (void)display;
    (void)mode;
    return 0;
}

static void PHOENIX_VideoQuit(_THIS)
{
    PHOENIX_VideoData *data = (PHOENIX_VideoData *)_this->driverdata;

    /* Pan the display back to buffer 0 (where fbcon draws) and hand the
     * framebuffer back to the text console. */
    if (data->scanout) {
        v3d_phoenix_flip(0);
    }

    /* Release the input devices (restores cooked keyboard mode) before handing
     * the framebuffer back to the text console. */
    PHOENIX_InputQuit();

    PHOENIX_ConsoleSetMode(data, FBCON_ENABLED);

    if (data->tty_fd >= 0) {
        close(data->tty_fd);
        data->tty_fd = -1;
    }
    if (data->fb_fd >= 0) {
        close(data->fb_fd);
        data->fb_fd = -1;
    }
}

static int PHOENIX_CreateWindow(_THIS, SDL_Window *window)
{
    PHOENIX_VideoData *data = (PHOENIX_VideoData *)_this->driverdata;

    if (data->window) {
        return SDL_SetError("PHOENIX: only one window is supported");
    }
    data->window = window;

    /* Always fullscreen at the native mode, whatever geometry was requested. */
    window->x = 0;
    window->y = 0;
    window->w = (int)data->width;
    window->h = (int)data->height;
    window->flags |= SDL_WINDOW_FULLSCREEN;
    window->flags &= ~(SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED);

    /* Emit the visible + focused window events rather than pre-setting the flags.
     * SDL_SendWindowEvent (and SDL_Set*Focus) suppress a state-change event when
     * the matching flag is ALREADY set, so manually OR-ing SDL_WINDOW_SHOWN /
     * _INPUT_FOCUS / _MOUSE_FOCUS here swallows the SHOWN / FOCUS_GAINED / ENTER
     * events. Apps that gate rendering on those events then break: e.g. quake3e
     * starts with gw_minimized = qtrue and clears it only on SHOWN / RESTORED /
     * FOCUS_GAINED; without the event, R_IssueRenderCommands skips the backend
     * ("skip backend when minimized") every frame and it renders black. Let SDL
     * set the flags AND deliver the events. */
    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_SHOWN, 0, 0);
    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);

    return 0;
}

static void PHOENIX_DestroyWindow(_THIS, SDL_Window *window)
{
    PHOENIX_VideoData *data = (PHOENIX_VideoData *)_this->driverdata;

    if (data->window == window) {
        data->window = NULL;
    }
}

#endif /* SDL_VIDEO_DRIVER_PHOENIX */

/* vi: set ts=4 sw=4 expandtab: */
