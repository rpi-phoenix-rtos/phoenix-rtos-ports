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

#ifdef SDL_INPUT_PHOENIX

/*
 * Phoenix-RTOS keyboard + mouse for the KMSDRM video driver (which on Linux
 * uses evdev; Phoenix has no evdev). The report handling is the one the
 * old-lane phoenix video driver proved on hardware (ports/sdl2 overlay
 * src/video/phoenix/SDL_phoenixevents.c), minus its driver-private window
 * pointer: events go to the window that has keyboard / mouse focus, which
 * KMSDRM_CreateWindow sets.
 *
 * KEYBOARD (/dev/kbd0): usbkbd is switched to RAW mode (write a 1 byte) and
 * then delivers 8-byte HID boot-keyboard reports: [0] modifier bitmask,
 * [1] reserved, [2..7] up to 6 pressed HID usages. Successive reports are
 * diffed into key down/up events. SDL_Scancode values ARE the USB HID usage
 * ids within the keyboard page (SDL_SCANCODE_A == 4, SDL_SCANCODE_LCTRL ==
 * 224), so the mapping is the identity.
 *
 * MOUSE (/dev/mouse0): 4-byte HID boot-mouse packets: [0] buttons (bit0 L,
 * bit1 R, bit2 M), [1] X int8, [2] Y int8, [3] wheel int8 -> relative motion,
 * button transitions, wheel.
 *
 * poll() does not wake on these nodes, so they are never blocked on: Poll()
 * drains them non-blocking with a bounded number of reads per call.
 *
 * DEVICE OWNERSHIP: pl011-tty's console bridge holds /dev/kbd0 while the HDMI
 * console is in text mode. On the new lane the console is handed over by
 * rpi4-kms -C (FBCONSETMODE(DISABLED) while a plane is shown). Without -C the
 * open keeps failing and the keyboard stays silent; the open is retried for a
 * bounded number of Poll() calls so a late handover is still picked up.
 */

#include "SDL_phoenixhid.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

#define PHOENIX_HID_KBD_PATH       "/dev/kbd0"
#define PHOENIX_HID_MOUSE_PATH     "/dev/mouse0"
#define PHOENIX_HID_MAX_OPEN_TRIES 200 /* Poll() calls: a few seconds of frames */
#define PHOENIX_HID_MAX_READS      64  /* reads per device per Poll() */

static int hid_kbd_fd = -1;
static int hid_mouse_fd = -1;
static int hid_kbd_raw = 0; /* 1 = usbkbd delivers raw 8-byte reports */
static uint8_t hid_kbd_prev[8];
static int hid_mouse_btn_prev = 0;
static int hid_kbd_tries = 0;
static int hid_mouse_tries = 0;

static void hid_open(void)
{
    if (hid_kbd_fd < 0 && hid_kbd_tries < PHOENIX_HID_MAX_OPEN_TRIES) {
        hid_kbd_tries++;
        hid_kbd_fd = open(PHOENIX_HID_KBD_PATH, O_RDWR | O_NONBLOCK);
        if (hid_kbd_fd >= 0) {
            unsigned char raw = 1u; /* ask usbkbd for raw 8-byte HID reports */
            hid_kbd_raw = (write(hid_kbd_fd, &raw, 1) == 1) ? 1 : 0;
            SDL_memset(hid_kbd_prev, 0, sizeof(hid_kbd_prev));
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "phoenix-hid: %s open after %d tries raw=%d",
                         PHOENIX_HID_KBD_PATH, hid_kbd_tries, hid_kbd_raw);
        }
    }
    if (hid_mouse_fd < 0 && hid_mouse_tries < PHOENIX_HID_MAX_OPEN_TRIES) {
        hid_mouse_tries++;
        hid_mouse_fd = open(PHOENIX_HID_MOUSE_PATH, O_RDONLY | O_NONBLOCK);
        if (hid_mouse_fd >= 0) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "phoenix-hid: %s open after %d tries",
                         PHOENIX_HID_MOUSE_PATH, hid_mouse_tries);
        }
    }
}

/* Is HID usage u present in the 6-slot key array of report rep[]? */
static int hid_usage_in(const uint8_t *rep, uint8_t u)
{
    int i;
    for (i = 2; i < 8; ++i) {
        if (rep[i] == u) {
            return 1;
        }
    }
    return 0;
}

static SDL_Scancode hid_scancode(uint8_t u)
{
    if (u >= 4 && u < SDL_NUM_SCANCODES) {
        return (SDL_Scancode)u;
    }
    return SDL_SCANCODE_UNKNOWN;
}

/* HID usage + modifier byte -> printable US-QWERTY character for SDL_TEXTINPUT,
 * or 0. SDL_SendKeyboardKey delivers scancodes only; text fields and game
 * consoles read SDL_TEXTINPUT, which the platform has to generate. Modifier
 * bits: 0 LCTRL 1 LSHIFT 2 LALT 3 LGUI 4 RCTRL 5 RSHIFT 6 RALT 7 RGUI; no text
 * while CTRL/ALT/GUI is held (shortcuts), SHIFT selects the shifted glyph. */
static char hid_to_char(uint8_t u, uint8_t mod)
{
    int shift = (mod & 0x22u) != 0;

    if ((mod & (0x11u | 0x44u | 0x88u)) != 0) {
        return 0;
    }
    if (u >= 0x04u && u <= 0x1du) { /* a..z */
        char c = (char)('a' + (int)(u - 0x04u));
        return shift ? (char)(c - 32) : c;
    }
    if (u >= 0x1eu && u <= 0x26u) { /* 1..9 */
        static const char sym[] = "!@#$%^&*(";
        return shift ? sym[u - 0x1eu] : (char)('1' + (int)(u - 0x1eu));
    }
    switch (u) {
    case 0x27u: return shift ? ')' : '0';
    case 0x2cu: return ' ';
    case 0x2du: return shift ? '_' : '-';
    case 0x2eu: return shift ? '+' : '=';
    case 0x2fu: return shift ? '{' : '[';
    case 0x30u: return shift ? '}' : ']';
    case 0x31u: return shift ? '|' : '\\';
    case 0x33u: return shift ? ':' : ';';
    case 0x34u: return shift ? '"' : '\'';
    case 0x35u: return shift ? '~' : '`';
    case 0x36u: return shift ? '<' : ',';
    case 0x37u: return shift ? '>' : '.';
    case 0x38u: return shift ? '?' : '/';
    default:    return 0;
    }
}

/* Diff one raw 8-byte keyboard report against the previous one. */
static void hid_kbd_process(const uint8_t *rep)
{
    uint8_t mod = rep[0], pmod = hid_kbd_prev[0];
    int bit, i;

    /* Modifier bits 0..7 = HID usages 0xE0..0xE7 = SDL scancodes LCTRL..RGUI. */
    for (bit = 0; bit < 8; ++bit) {
        int now = (mod >> bit) & 1;
        int was = (pmod >> bit) & 1;
        if (now != was) {
            SDL_SendKeyboardKey(now ? SDL_PRESSED : SDL_RELEASED, (SDL_Scancode)(SDL_SCANCODE_LCTRL + bit));
        }
    }

    for (i = 2; i < 8; ++i) { /* key-ups: in the old report, not in the new */
        uint8_t u = hid_kbd_prev[i];
        if (u >= 4 && !hid_usage_in(rep, u)) {
            SDL_Scancode sc = hid_scancode(u);
            if (sc != SDL_SCANCODE_UNKNOWN) {
                SDL_SendKeyboardKey(SDL_RELEASED, sc);
            }
        }
    }
    for (i = 2; i < 8; ++i) { /* key-downs: new in this report */
        uint8_t u = rep[i];
        if (u >= 4 && !hid_usage_in(hid_kbd_prev, u)) {
            SDL_Scancode sc = hid_scancode(u);
            char c;
            if (sc != SDL_SCANCODE_UNKNOWN) {
                SDL_SendKeyboardKey(SDL_PRESSED, sc);
            }
            c = hid_to_char(u, mod);
            if (c != 0) { /* delivered only while text input is active */
                char text[2];
                text[0] = c;
                text[1] = '\0';
                SDL_SendKeyboardText(text);
            }
        }
    }

    SDL_memcpy(hid_kbd_prev, rep, 8);
}

/* One raw 4-byte mouse packet: relative motion, buttons, wheel. */
static void hid_mouse_process(const uint8_t *p)
{
    SDL_Window *window = SDL_GetMouseFocus();
    int btn = p[0];
    int dx = (int)(signed char)p[1];
    int dy = (int)(signed char)p[2];
    int wheel = (int)(signed char)p[3];

    if (dx || dy) {
        SDL_SendMouseMotion(window, 0, 1 /* relative */, dx, dy);
    }
    if ((btn & 0x01) != (hid_mouse_btn_prev & 0x01)) {
        SDL_SendMouseButton(window, 0, (btn & 0x01) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_LEFT);
    }
    if ((btn & 0x02) != (hid_mouse_btn_prev & 0x02)) {
        SDL_SendMouseButton(window, 0, (btn & 0x02) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_RIGHT);
    }
    if ((btn & 0x04) != (hid_mouse_btn_prev & 0x04)) {
        SDL_SendMouseButton(window, 0, (btn & 0x04) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_MIDDLE);
    }
    if (wheel) {
        SDL_SendMouseWheel(window, 0, 0.0f, (float)wheel, SDL_MOUSEWHEEL_NORMAL);
    }
    hid_mouse_btn_prev = btn;
}

void SDL_PHOENIX_HID_Init(void)
{
    hid_kbd_tries = 0;
    hid_mouse_tries = 0;
    hid_mouse_btn_prev = 0;
    hid_open();
}

void SDL_PHOENIX_HID_Poll(void)
{
    unsigned char buf[64];
    ssize_t r;
    int off, guard;

    hid_open();

    if (hid_kbd_fd >= 0 && hid_kbd_raw) {
        for (guard = 0; guard < PHOENIX_HID_MAX_READS && (r = read(hid_kbd_fd, buf, sizeof(buf))) > 0; ++guard) {
            for (off = 0; off + 8 <= (int)r; off += 8) {
                hid_kbd_process(buf + off);
            }
        }
    }
    if (hid_mouse_fd >= 0) {
        for (guard = 0; guard < PHOENIX_HID_MAX_READS && (r = read(hid_mouse_fd, buf, sizeof(buf))) > 0; ++guard) {
            for (off = 0; off + 4 <= (int)r; off += 4) {
                hid_mouse_process(buf + off);
            }
        }
    }
}

void SDL_PHOENIX_HID_Quit(void)
{
    if (hid_kbd_fd >= 0) {
        if (hid_kbd_raw) {
            unsigned char cooked = 0u; /* hand cooked mode back to the console bridge */
            (void)write(hid_kbd_fd, &cooked, 1);
        }
        close(hid_kbd_fd);
        hid_kbd_fd = -1;
    }
    if (hid_mouse_fd >= 0) {
        close(hid_mouse_fd);
        hid_mouse_fd = -1;
    }
    hid_kbd_raw = 0;
}

#endif /* SDL_INPUT_PHOENIX */

/* vi: set ts=4 sw=4 expandtab: */
