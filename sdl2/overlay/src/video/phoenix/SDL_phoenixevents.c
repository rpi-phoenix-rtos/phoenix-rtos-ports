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
 * Phoenix-RTOS input: SDL input lives in the video driver's PumpEvents.
 *
 * KEYBOARD (/dev/kbd0). usbkbd is switched to RAW mode (write a 1 byte) so it
 * delivers raw 8-byte HID boot-keyboard reports: [0]=modifier bitmask,
 * [1]=reserved, [2..7]=up to 6 pressed HID usages. We diff successive reports
 * into SDL key-down/up events. Conveniently, SDL_Scancode values ARE the USB
 * HID usage IDs (SDL_SCANCODE_A == 4 == HID 0x04; SDL_SCANCODE_LCTRL == 224 ==
 * HID 0xE0), so the mapping is the identity within the keyboard usage page.
 *
 * MOUSE (/dev/mouse0). Raw 4-byte HID boot-mouse packets: [0]=buttons
 * (bit0 L, bit1 R, bit2 M), [1]=X int8, [2]=Y int8, [3]=wheel int8. Motion is
 * fed as relative SDL_MOUSEMOTION; button transitions as SDL_MOUSEBUTTON*.
 *
 * poll() GOTCHA: Phoenix poll() does not wake on these HID fds. We therefore
 * never block on them — PumpEvents drains non-blocking with a per-frame bounded
 * guard (a device streaming faster than we drain can never spin us forever).
 *
 * DEVICE OWNERSHIP: /dev/kbd0 is normally held by pl011-tty's console bridge;
 * it releases it once the HDMI console switches to graphics mode (which
 * VideoInit does via FBCONSETMODE(FBCON_DISABLED)). The open is retried across
 * the first few PumpEvents calls to absorb that async release, without ever
 * sleeping in the frame loop.
 */

#include "../../events/SDL_events_c.h"

#include "SDL_phoenixvideo.h"
#include "SDL_phoenixevents.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

static int phoenix_kbd_fd = -1;
static int phoenix_mouse_fd = -1;
static int phoenix_kbd_raw = 0;    /* 1 = usbkbd raw 8-byte HID reports active */
static uint8_t phoenix_kbd_prev[8];
static int phoenix_mouse_btn_prev;
static int phoenix_input_tries = 0; /* bounded lazy-open attempts (keyboard) */
static int phoenix_mouse_tries = 0; /* bounded lazy-open attempts (mouse) */

#define PHOENIX_INPUT_MAX_OPEN_TRIES 200 /* ~a few seconds of frames */

static void PHOENIX_InputOpen(void)
{
    if (phoenix_kbd_fd < 0 && phoenix_input_tries < PHOENIX_INPUT_MAX_OPEN_TRIES) {
        phoenix_input_tries++;
        phoenix_kbd_fd = open("/dev/kbd0", O_RDWR | O_NONBLOCK);
        if (phoenix_kbd_fd >= 0) {
            unsigned char raw = 1u; /* ask usbkbd for raw 8-byte HID reports */
            phoenix_kbd_raw = (write(phoenix_kbd_fd, &raw, 1) == 1) ? 1 : 0;
            memset(phoenix_kbd_prev, 0, sizeof(phoenix_kbd_prev));
        }
    }
    if (phoenix_mouse_fd < 0 && phoenix_mouse_tries < PHOENIX_INPUT_MAX_OPEN_TRIES) {
        phoenix_mouse_tries++;
        phoenix_mouse_fd = open("/dev/mouse0", O_RDONLY | O_NONBLOCK);
    }
}

/* Is HID usage u present in the 6-slot key array of report rep[]? */
static int phoenix_usage_in(const uint8_t *rep, uint8_t u)
{
    int i;
    for (i = 2; i < 8; ++i) {
        if (rep[i] == u) {
            return 1;
        }
    }
    return 0;
}

/* HID usage -> SDL_Scancode (identity within the keyboard usage page). */
static SDL_Scancode phoenix_hid_scancode(uint8_t u)
{
    if (u >= 4 && u < SDL_NUM_SCANCODES) {
        return (SDL_Scancode)u;
    }
    return SDL_SCANCODE_UNKNOWN;
}

/* Diff one raw 8-byte HID report against the previous one -> SDL key events. */
static void phoenix_kbd_process(const uint8_t *rep)
{
    uint8_t mod = rep[0], pmod = phoenix_kbd_prev[0];
    int bit;
    int i;

    /* Modifier byte bits 0..7 map to HID usages 0xE0..0xE7, i.e. SDL scancodes
     * 224..231 (LCTRL..RGUI). Emit transitions. */
    for (bit = 0; bit < 8; ++bit) {
        int now = (mod >> bit) & 1;
        int was = (pmod >> bit) & 1;
        if (now != was) {
            SDL_SendKeyboardKey(now ? SDL_PRESSED : SDL_RELEASED,
                                (SDL_Scancode)(SDL_SCANCODE_LCTRL + bit));
        }
    }

    /* Key-ups: usages in the old report no longer present. */
    for (i = 2; i < 8; ++i) {
        uint8_t u = phoenix_kbd_prev[i];
        if (u >= 4 && !phoenix_usage_in(rep, u)) {
            SDL_Scancode sc = phoenix_hid_scancode(u);
            if (sc != SDL_SCANCODE_UNKNOWN) {
                SDL_SendKeyboardKey(SDL_RELEASED, sc);
            }
        }
    }
    /* Key-downs: usages newly present. */
    for (i = 2; i < 8; ++i) {
        uint8_t u = rep[i];
        if (u >= 4 && !phoenix_usage_in(phoenix_kbd_prev, u)) {
            SDL_Scancode sc = phoenix_hid_scancode(u);
            if (sc != SDL_SCANCODE_UNKNOWN) {
                SDL_SendKeyboardKey(SDL_PRESSED, sc);
            }
        }
    }

    memcpy(phoenix_kbd_prev, rep, 8);
}

/* Parse one raw 4-byte HID mouse packet: relative motion + button + wheel. */
static void phoenix_mouse_process(SDL_Window *window, const uint8_t *p)
{
    int btn = p[0];
    int dx = (int)(signed char)p[1];
    int dy = (int)(signed char)p[2];
    int wheel = (int)(signed char)p[3];

    if (dx || dy) {
        SDL_SendMouseMotion(window, 0, 1 /* relative */, dx, dy);
    }
    if ((btn & 0x01) != (phoenix_mouse_btn_prev & 0x01)) {
        SDL_SendMouseButton(window, 0, (btn & 0x01) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_LEFT);
    }
    if ((btn & 0x02) != (phoenix_mouse_btn_prev & 0x02)) {
        SDL_SendMouseButton(window, 0, (btn & 0x02) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_RIGHT);
    }
    if ((btn & 0x04) != (phoenix_mouse_btn_prev & 0x04)) {
        SDL_SendMouseButton(window, 0, (btn & 0x04) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_MIDDLE);
    }
    if (wheel) {
        SDL_SendMouseWheel(window, 0, 0.0f, (float)wheel, SDL_MOUSEWHEEL_NORMAL);
    }
    phoenix_mouse_btn_prev = btn;
}

void PHOENIX_InputQuit(void)
{
    if (phoenix_kbd_fd >= 0) {
        if (phoenix_kbd_raw) {
            unsigned char cooked = 0u; /* restore cooked mode for the console bridge */
            (void)write(phoenix_kbd_fd, &cooked, 1);
        }
        close(phoenix_kbd_fd);
        phoenix_kbd_fd = -1;
    }
    if (phoenix_mouse_fd >= 0) {
        close(phoenix_mouse_fd);
        phoenix_mouse_fd = -1;
    }
    phoenix_kbd_raw = 0;
    phoenix_input_tries = 0;
}

void PHOENIX_PumpEvents(_THIS)
{
    PHOENIX_VideoData *data = (PHOENIX_VideoData *)_this->driverdata;
    SDL_Window *window = data->window;
    unsigned char buf[64];
    ssize_t r;
    int off;
    int guard;

    PHOENIX_InputOpen();

    /* BOUNDED drains: cap reads per frame so a device delivering a continuous
     * stream (held key auto-repeat, a noisy mouse) can never spin these loops
     * indefinitely and stall the host frame loop. O_NONBLOCK returns -1 when
     * the fifo empties; the guard bounds a refilled-faster-than-drained fifo. */
    if (phoenix_kbd_fd >= 0 && phoenix_kbd_raw) {
        for (guard = 0; guard < 64 && (r = read(phoenix_kbd_fd, buf, sizeof(buf))) > 0; ++guard) {
            for (off = 0; off + 8 <= (int)r; off += 8) {
                phoenix_kbd_process(buf + off);
            }
        }
    }

    if (phoenix_mouse_fd >= 0) {
        for (guard = 0; guard < 64 && (r = read(phoenix_mouse_fd, buf, sizeof(buf))) > 0; ++guard) {
            for (off = 0; off + 4 <= (int)r; off += 4) {
                phoenix_mouse_process(window, buf + off);
            }
        }
    }
}

#endif /* SDL_VIDEO_DRIVER_PHOENIX */

/* vi: set ts=4 sw=4 expandtab: */
