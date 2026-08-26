/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * Phoenix-RTOS platform backend for vkQuake (vkQuake is Copyright (C) id
 * Software, Inc. and the vkQuake developers, GPL-2.0-or-later). It implements
 * the vkQuake platform interface and is distributed under the same license as
 * the program it is built into; see COPYING in this directory.
 */
/*
 * pl_phoenix_in.c — Phoenix input shim for vkQuake: replaces in_sdl.c.
 * (Adapted from the quakespasm-port shim; identical /dev/kbd0 + /dev/mouse0 decoders.
 * vkQuake additions: SDL_GetMouseState, which menu.c calls directly to detect mouse
 * movement in menus — we track an absolute pixel position from the relative HID deltas.)
 *
 * Drives Quake input from the USB keyboard (/dev/kbd0) and mouse (/dev/mouse0).
 *
 * KEYBOARD. We put usbkbd into RAW mode (write a 1 byte to /dev/kbd0; see usbkbd.c)
 * so it delivers raw 8-byte HID boot-keyboard reports instead of the cooked ASCII
 * stream. Raw reports carry full key state every change, so by diffing successive
 * reports we generate real key-DOWN and key-UP events — which Quake needs for
 * held-key movement (+forward latches on down, releases on up). Cooked ASCII cannot
 * express key-up, so it only suited menus/console; raw mode makes the keyboard
 * playable. If raw mode can't be enabled (older usbkbd), we fall back to the cooked
 * decoder below (menu/console only, no sustained movement).
 *   HID report: [0]=modifier bitmask, [1]=reserved, [2..7]=up to 6 pressed usages.
 *
 * MOUSE. /dev/mouse0 delivers raw 4-byte HID boot-mouse packets on change:
 *   [0]=buttons (bit0 L, bit1 R, bit2 M), [1]=X int8, [2]=Y int8, [3]=wheel int8.
 * We accumulate relative motion into total_dx/total_dy (applied in IN_Move, the same
 * math as in_sdl.c's IN_MouseMove) and turn button transitions into K_MOUSE1/2/3.
 *
 * Quake input entry points (mirroring in_sdl.c's SDL_KEYDOWN / SDL_TEXTINPUT):
 *   - Key_Event(quakekey, down)  -> bindings, movement, menu nav, console control
 *   - Char_Event(ascii)          -> console / message text entry
 *   - IN_Move(cmd)               -> apply accumulated mouse motion to the usercmd
 *
 * DEVICE OWNERSHIP. /dev/kbd0 is normally held by pl011-tty's console bridge
 * (single-opener usbkbd). pl011-tty now releases it when the HDMI console switches
 * to graphics mode (FBCONSETMODE(FBCON_DISABLED), which VID_Init does before us), so
 * our open() succeeds — we retry briefly to absorb the async release. /dev/mouse0 is
 * not held by anyone. On shutdown we close both (pl011-tty reacquires the keyboard).
 */
#include "quakedef.h"
#include "SDL.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

static int kbd_fd = -1;
static int mouse_fd = -1;
static int kbd_raw = 0;                 /* 1 = usbkbd raw 8-byte HID reports active */
static uint8_t kbd_prev[8];             /* previous raw report, for down/up diffing */
static int total_dx, total_dy;          /* accumulated mouse motion (applied in IN_Move) */
static int mouse_btn_prev;              /* previous mouse button bitmask */
static int mouse_abs_x, mouse_abs_y;    /* absolute pixel position (for SDL_GetMouseState) */
static int mouse_cur_btn;               /* current mouse button bitmask (for SDL_GetMouseState) */

/* Pitch-clamp cvars (defined in view.c); in_sdl.c declares these locally too. */
extern cvar_t cl_maxpitch;
extern cvar_t cl_minpitch;

/* Emit one decoded key. quakekey = Quake key code (K_* or lowercase ascii for
 * letter/printable keys); ch = the printable ascii to feed Char_Event, or 0. */
static void pl_in_emit(int quakekey, int ch)
{
	if (quakekey <= 0)
		return;
	Key_Event(quakekey, true);
	if (ch > 0 && ch < 128)
		Char_Event(ch);
	Key_Event(quakekey, false);
}

/*
 * Decode a cooked kbd byte buffer into Quake key events, dispatched through emit().
 * Factored out (emit as a parameter) so the escape-sequence decoding can be
 * unit-tested at boot without real hardware (see pl_in_selftest).
 */
static void pl_in_decode(const unsigned char *b, int n, void (*emit)(int, int))
{
	int i = 0;

	while (i < n) {
		unsigned char c = b[i];

		if (c == 0x1bu) {		/* ESC: standalone, or start of a CSI sequence */
			if (i + 1 < n && b[i + 1] == '[') {
				unsigned char k = (i + 2 < n) ? b[i + 2] : 0u;
				int key = 0;
				int seqlen = 3;	/* ESC '[' <final> */

				switch (k) {
				case 'A': key = K_UPARROW; break;
				case 'B': key = K_DOWNARROW; break;
				case 'C': key = K_RIGHTARROW; break;
				case 'D': key = K_LEFTARROW; break;
				case 'H': key = K_HOME; break;
				case 'F': key = K_END; break;
				/* "ESC [ <digit> ~" forms (driver: Del/PgDn/etc.) */
				case '3': key = K_DEL;  seqlen = 4; break;
				case '4': key = K_END;  seqlen = 4; break;
				case '5': key = K_PGUP; seqlen = 4; break;
				case '6': key = K_PGDN; seqlen = 4; break;
				default:  key = 0;      break;
				}
				emit(key, 0);
				i += seqlen;
				continue;
			}
			emit(K_ESCAPE, 0);
			i += 1;
			continue;
		}

		if (c == '\n' || c == '\r') { emit(K_ENTER, 0); i++; continue; }
		if (c == '\b' || c == 0x7fu) { emit(K_BACKSPACE, 0); i++; continue; }
		if (c == '\t') { emit(K_TAB, 0); i++; continue; }

		if (c >= 0x20u && c <= 0x7eu) {		/* printable: key=lowercase, char=as-typed */
			int key = c;
			if (key >= 'A' && key <= 'Z')
				key = key - 'A' + 'a';
			emit(key, c);
			i++;
			continue;
		}

		i++;	/* ignore other control bytes (raw ctrl chars, etc.) */
	}
}

/* --- boot-time self-test of the decoder (no hardware needed) --- */
static int       st_n;
static int       st_keys[16];
static void st_emit(int key, int ch) { (void)ch; if (st_n < 16) st_keys[st_n++] = key; }

static int st_check(const char *in, int len, const int *want, int wantn)
{
	int i;
	st_n = 0;
	pl_in_decode((const unsigned char *)in, len, st_emit);
	if (st_n != wantn)
		return 0;
	for (i = 0; i < wantn; i++)
		if (st_keys[i] != want[i])
			return 0;
	return 1;
}

static void pl_in_selftest(void)
{
	int pass = 0, total = 0;
	struct { const char *in; int len; int want[4]; int wantn; } cases[] = {
		{ "w",        1, { 'w' },                 1 },
		{ "AB",       2, { 'a', 'b' },            2 },	/* uppercase -> lowercase key */
		{ "\033",     1, { K_ESCAPE },            1 },
		{ "\033[A",   3, { K_UPARROW },           1 },
		{ "\033[B",   3, { K_DOWNARROW },         1 },
		{ "\033[C",   3, { K_RIGHTARROW },        1 },
		{ "\033[D",   3, { K_LEFTARROW },         1 },
		{ "\n",       1, { K_ENTER },             1 },
		{ "\b",       1, { K_BACKSPACE },         1 },
		{ "\033[3~",  4, { K_DEL },               1 },
		{ "a\033[A\n",5, { 'a', K_UPARROW, K_ENTER }, 3 },	/* mixed in one read */
	};
	int i, nc = (int)(sizeof(cases) / sizeof(cases[0]));

	for (i = 0; i < nc; i++) {
		total++;
		if (st_check(cases[i].in, cases[i].len, cases[i].want, cases[i].wantn))
			pass++;
		else
			Sys_Printf("PL_IN selftest FAIL case %d\n", i);
	}
	Sys_Printf("PL_IN: kbd decoder selftest %d/%d passed\n", pass, total);
}

/* --- raw HID keyboard: map a HID usage (page 0x07) to a Quake key, 0 if none --- */
static int pl_hid_key(uint8_t u)
{
	if (u >= 0x04u && u <= 0x1du)
		return 'a' + (u - 0x04u);          /* a..z -> lowercase ascii */
	if (u >= 0x1eu && u <= 0x26u)
		return '1' + (u - 0x1eu);          /* 1..9 */
	if (u >= 0x3au && u <= 0x45u)
		return K_F1 + (u - 0x3au);         /* F1..F12 */
	switch (u) {
	case 0x27: return '0';
	case 0x28: return K_ENTER;
	case 0x29: return K_ESCAPE;
	case 0x2a: return K_BACKSPACE;
	case 0x2b: return K_TAB;
	case 0x2c: return K_SPACE;
	case 0x2d: return '-';  case 0x2e: return '=';  case 0x2f: return '[';
	case 0x30: return ']';  case 0x31: return '\\'; case 0x33: return ';';
	case 0x34: return '\''; case 0x35: return '`';  case 0x36: return ',';
	case 0x37: return '.';  case 0x38: return '/';
	case 0x49: return K_INS;  case 0x4a: return K_HOME; case 0x4b: return K_PGUP;
	case 0x4c: return K_DEL;  case 0x4d: return K_END;  case 0x4e: return K_PGDN;
	case 0x4f: return K_RIGHTARROW; case 0x50: return K_LEFTARROW;
	case 0x51: return K_DOWNARROW;  case 0x52: return K_UPARROW;
	default:   return 0;
	}
}

/* Printable ascii for Char_Event (text entry), honoring shift. 0 if non-printable. */
static int pl_hid_char(uint8_t u, int shift)
{
	if (u >= 0x04u && u <= 0x1du) {
		int c = 'a' + (u - 0x04u);
		return shift ? (c - 'a' + 'A') : c;
	}
	if (u >= 0x1eu && u <= 0x26u) {
		static const char base[] = "123456789";
		static const char sh[]   = "!@#$%^&*(";
		return shift ? sh[u - 0x1eu] : base[u - 0x1eu];
	}
	switch (u) {
	case 0x27: return shift ? ')' : '0';
	case 0x2c: return ' ';
	case 0x2d: return shift ? '_' : '-';   case 0x2e: return shift ? '+' : '=';
	case 0x2f: return shift ? '{' : '[';   case 0x30: return shift ? '}' : ']';
	case 0x31: return shift ? '|' : '\\';  case 0x33: return shift ? ':' : ';';
	case 0x34: return shift ? '"' : '\'';  case 0x35: return shift ? '~' : '`';
	case 0x36: return shift ? '<' : ',';   case 0x37: return shift ? '>' : '.';
	case 0x38: return shift ? '?' : '/';
	default:   return 0;
	}
}

static int pl_usage_in(const uint8_t *rep, uint8_t u)
{
	int i;
	for (i = 2; i < 8; ++i)
		if (rep[i] == u)
			return 1;
	return 0;
}

static void pl_emit_mod(int key, int now, int was)
{
	if (now && !was)
		Key_Event(key, true);
	else if (!now && was)
		Key_Event(key, false);
}

/* Diff one raw 8-byte HID report against the previous one -> key down/up events. */
static void pl_kbd_raw_process(const uint8_t *rep)
{
	uint8_t mod = rep[0], pmod = kbd_prev[0];
	int shift = (mod & (0x02u | 0x20u)) != 0;   /* L or R shift */
	int i;

	/* modifier transitions (L+R fold to one Quake key, so compare aggregates) */
	pl_emit_mod(K_CTRL,  (mod & 0x11u), (pmod & 0x11u));
	pl_emit_mod(K_SHIFT, (mod & 0x22u), (pmod & 0x22u));
	pl_emit_mod(K_ALT,   (mod & 0x44u), (pmod & 0x44u));

	/* key-ups: usages in the old report no longer present */
	for (i = 2; i < 8; ++i) {
		uint8_t u = kbd_prev[i];
		if (u >= 0x04u && !pl_usage_in(rep, u)) {
			int k = pl_hid_key(u);
			if (k)
				Key_Event(k, false);
		}
	}
	/* key-downs: usages newly present */
	for (i = 2; i < 8; ++i) {
		uint8_t u = rep[i];
		if (u >= 0x04u && !pl_usage_in(kbd_prev, u)) {
			int k = pl_hid_key(u);
			if (k) {
				int ch = pl_hid_char(u, shift);
				Key_Event(k, true);
				if (ch)
					Char_Event(ch);
			}
		}
	}

	memcpy(kbd_prev, rep, 8);
}

/* Parse one raw 4-byte HID mouse packet: accumulate motion + button transitions. */
static void pl_mouse_process(const uint8_t *p)
{
	int btn = p[0];

	total_dx += (int)(signed char)p[1];
	total_dy += (int)(signed char)p[2];

	/* Maintain an absolute pixel position for SDL_GetMouseState (menu mouse). vid.width/
	 * vid.height come from the vid shim; clamp into the current mode so the menu cursor
	 * stays on-screen. */
	mouse_abs_x += (int)(signed char)p[1];
	mouse_abs_y += (int)(signed char)p[2];
	if (mouse_abs_x < 0)
		mouse_abs_x = 0;
	else if (vid.width > 0 && mouse_abs_x >= (int)vid.width)
		mouse_abs_x = (int)vid.width - 1;
	if (mouse_abs_y < 0)
		mouse_abs_y = 0;
	else if (vid.height > 0 && mouse_abs_y >= (int)vid.height)
		mouse_abs_y = (int)vid.height - 1;
	mouse_cur_btn = btn;

	pl_emit_mod(K_MOUSE1, (btn & 0x01), (mouse_btn_prev & 0x01));
	pl_emit_mod(K_MOUSE2, (btn & 0x02), (mouse_btn_prev & 0x02));
	pl_emit_mod(K_MOUSE3, (btn & 0x04), (mouse_btn_prev & 0x04));
	mouse_btn_prev = btn;
}

void IN_Init(void)
{
	int tries;
	unsigned char raw = 1u;

	pl_in_selftest();

	/* /dev/kbd0 may still be held by the pl011-tty console bridge for a moment after
	 * VID_Init switched the HDMI console to graphics mode (the bridge releases it
	 * asynchronously); retry briefly to absorb that race. */
	for (tries = 0; tries < 40; ++tries) {
		kbd_fd = open("/dev/kbd0", O_RDWR | O_NONBLOCK);
		if (kbd_fd >= 0)
			break;
		usleep(25000);
	}
	if (kbd_fd < 0) {
		Sys_Printf("PL_IN: /dev/kbd0 open failed (errno=%d) — keyboard input disabled\n", errno);
	}
	else {
		/* Ask usbkbd for raw 8-byte HID reports (key-up + held state). */
		if (write(kbd_fd, &raw, 1) == 1) {
			kbd_raw = 1;
			Sys_Printf("PL_IN: /dev/kbd0 opened (fd=%d), RAW HID mode — keyboard active\n", kbd_fd);
		}
		else {
			kbd_raw = 0;
			Sys_Printf("PL_IN: /dev/kbd0 opened (fd=%d), cooked mode (raw unavailable) — keyboard active\n", kbd_fd);
		}
		memset(kbd_prev, 0, sizeof(kbd_prev));
	}

	mouse_fd = open("/dev/mouse0", O_RDONLY | O_NONBLOCK);
	if (mouse_fd < 0)
		Sys_Printf("PL_IN: /dev/mouse0 open failed (errno=%d) — mouse input disabled\n", errno);
	else
		Sys_Printf("PL_IN: /dev/mouse0 opened (fd=%d) — mouse active\n", mouse_fd);
}

void IN_Shutdown(void)
{
	if (kbd_fd >= 0) {
		unsigned char cooked = 0u;
		if (kbd_raw)
			(void)write(kbd_fd, &cooked, 1);   /* restore cooked for the console bridge */
		close(kbd_fd);
		kbd_fd = -1;
	}
	if (mouse_fd >= 0) {
		close(mouse_fd);
		mouse_fd = -1;
	}
}

void IN_Commands(void)
{
}

/* Apply accumulated mouse motion to the player command — identical math to in_sdl.c. */
void IN_Move(usercmd_t *cmd)
{
	float dmx = total_dx * sensitivity.value;
	float dmy = total_dy * sensitivity.value;

	total_dx = 0;
	total_dy = 0;

	if (cl.paused || key_dest != key_game)
		return;

	if ((in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1)))
		cmd->sidemove += m_side.value * dmx;
	else
		cl.viewangles[YAW] -= m_yaw.value * dmx;

	if (in_mlook.state & 1) {
		if (dmx || dmy)
			V_StopPitchDrift();
	}

	if ((in_mlook.state & 1) && !(in_strafe.state & 1)) {
		cl.viewangles[PITCH] += m_pitch.value * dmy;
		if (cl.viewangles[PITCH] > cl_maxpitch.value)
			cl.viewangles[PITCH] = cl_maxpitch.value;
		if (cl.viewangles[PITCH] < cl_minpitch.value)
			cl.viewangles[PITCH] = cl_minpitch.value;
	}
	else {
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * dmy;
		else
			cmd->forwardmove -= m_forward.value * dmy;
	}
}

void IN_SendKeyEvents(void)
{
	unsigned char buf[64];
	ssize_t r;
	int off;
	int guard;

	/* BOUNDED drains: cap reads PER FRAME so a device delivering a continuous stream (a noisy/
	 * fast mouse, or a held key auto-repeating) can never spin these while() loops indefinitely
	 * and stall the host frame loop. 64 reads/frame = up to ~1024 mouse packets or ~512 kbd
	 * reports — far more than any real input burst; the rest drains next frame. (O_NONBLOCK already
	 * makes read() return -1/EWOULDBLOCK when empty, but only if the device fifo actually empties;
	 * a refilled-faster-than-drained fifo otherwise loops forever — this guard bounds that.) */
	if (kbd_fd >= 0) {
		if (kbd_raw) {
			/* raw 8-byte HID reports (packet-aligned by usbkbd) */
			for (guard = 0; guard < 64 && (r = read(kbd_fd, buf, sizeof(buf))) > 0; ++guard)
				for (off = 0; off + 8 <= (int)r; off += 8)
					pl_kbd_raw_process(buf + off);
		}
		else {
			for (guard = 0; guard < 64 && (r = read(kbd_fd, buf, sizeof(buf))) > 0; ++guard)
				pl_in_decode(buf, (int)r, pl_in_emit);
		}
	}

	if (mouse_fd >= 0) {
		/* raw 4-byte HID mouse packets (packet-aligned by usbmouse) */
		for (guard = 0; guard < 64 && (r = read(mouse_fd, buf, sizeof(buf))) > 0; ++guard)
			for (off = 0; off + 4 <= (int)r; off += 4)
				pl_mouse_process(buf + off);
	}
}

void IN_UpdateInputMode(void)
{
}

void IN_ClearStates(void)
{
}

void IN_Activate(void)
{
}

void IN_Deactivate(qboolean free_cursor)
{
	(void)free_cursor;
}

void IN_MouseMotion(int dx, int dy)
{
	(void)dx;
	(void)dy;
}

/*
 * SDL_GetMouseState (SDL2 int* form) — menu.c calls this directly (not through the input
 * platform TU) to detect in-menu mouse movement. Report the absolute pixel position we
 * track from the relative HID deltas, plus the SDL button bitmask (SDL_BUTTON(n) ==
 * 1<<(n-1): bit0 left, bit1 middle, bit2 right). Our HID byte is bit0 L, bit1 R, bit2 M,
 * so remap right<->middle to the SDL ordering.
 */
Uint32 SDL_GetMouseState(int *x, int *y)
{
	Uint32 m = 0;
	if (x != NULL)
		*x = mouse_abs_x;
	if (y != NULL)
		*y = mouse_abs_y;
	if (mouse_cur_btn & 0x01)
		m |= 0x1;                       /* left   -> SDL_BUTTON(1) */
	if (mouse_cur_btn & 0x04)
		m |= 0x2;                       /* middle -> SDL_BUTTON(2) */
	if (mouse_cur_btn & 0x02)
		m |= 0x4;                       /* right  -> SDL_BUTTON(3) */
	return m;
}
