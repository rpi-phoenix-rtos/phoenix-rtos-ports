/*
 * Phoenix-RTOS X11 fbdev DDX -- USB HID boot-keyboard usage -> evdev keycode map.
 *
 * The compiled-in X keymap is us/pc105 under the evdev rules, whose keycodes are
 * the Linux input-event-codes (evdev) ABI. This maps a HID keyboard-page (0x07)
 * usage to its evdev keycode; fbdev.c then adds the X keycode offset of 8.
 *
 * The usage->keycode mapping is reproduced from FreeBSD's evdev support
 * (sys/dev/evdev/evdev_utils.c, evdev_usb_scancodes[]), whose BSD-2-Clause notice
 * is retained below. The KEY_* values are the evdev input-event-codes (an ABI).
 */

/*-
 * Copyright (c) 2014 Jakub Wojciech Klama <jceel@FreeBSD.org>
 * Copyright (c) 2015-2016 Vladimir Kondratyev <wulf@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _FBDEV_HID_EVDEV_MAP_H_
#define _FBDEV_HID_EVDEV_MAP_H_

/* evdev input-event-codes (ABI) used by the table below; 0 == no key. */
#define KEY_ESC              1
#define KEY_1                2
#define KEY_2                3
#define KEY_3                4
#define KEY_4                5
#define KEY_5                6
#define KEY_6                7
#define KEY_7                8
#define KEY_8                9
#define KEY_9                10
#define KEY_0                11
#define KEY_MINUS            12
#define KEY_EQUAL            13
#define KEY_BACKSPACE        14
#define KEY_TAB              15
#define KEY_Q                16
#define KEY_W                17
#define KEY_E                18
#define KEY_R                19
#define KEY_T                20
#define KEY_Y                21
#define KEY_U                22
#define KEY_I                23
#define KEY_O                24
#define KEY_P                25
#define KEY_LEFTBRACE        26
#define KEY_RIGHTBRACE       27
#define KEY_ENTER            28
#define KEY_LEFTCTRL         29
#define KEY_A                30
#define KEY_S                31
#define KEY_D                32
#define KEY_F                33
#define KEY_G                34
#define KEY_H                35
#define KEY_J                36
#define KEY_K                37
#define KEY_L                38
#define KEY_SEMICOLON        39
#define KEY_APOSTROPHE       40
#define KEY_GRAVE            41
#define KEY_LEFTSHIFT        42
#define KEY_BACKSLASH        43
#define KEY_Z                44
#define KEY_X                45
#define KEY_C                46
#define KEY_V                47
#define KEY_B                48
#define KEY_N                49
#define KEY_M                50
#define KEY_COMMA            51
#define KEY_DOT              52
#define KEY_SLASH            53
#define KEY_RIGHTSHIFT       54
#define KEY_KPASTERISK       55
#define KEY_LEFTALT          56
#define KEY_SPACE            57
#define KEY_CAPSLOCK         58
#define KEY_F1               59
#define KEY_F2               60
#define KEY_F3               61
#define KEY_F4               62
#define KEY_F5               63
#define KEY_F6               64
#define KEY_F7               65
#define KEY_F8               66
#define KEY_F9               67
#define KEY_F10              68
#define KEY_NUMLOCK          69
#define KEY_SCROLLLOCK       70
#define KEY_KP7              71
#define KEY_KP8              72
#define KEY_KP9              73
#define KEY_KPMINUS          74
#define KEY_KP4              75
#define KEY_KP5              76
#define KEY_KP6              77
#define KEY_KPPLUS           78
#define KEY_KP1              79
#define KEY_KP2              80
#define KEY_KP3              81
#define KEY_KP0              82
#define KEY_KPDOT            83
#define KEY_ZENKAKUHANKAKU   85
#define KEY_102ND            86
#define KEY_F11              87
#define KEY_F12              88
#define KEY_RO               89
#define KEY_KATAKANA         90
#define KEY_HIRAGANA         91
#define KEY_HENKAN           92
#define KEY_KATAKANAHIRAGANA 93
#define KEY_MUHENKAN         94
#define KEY_KPJPCOMMA        95
#define KEY_KPENTER          96
#define KEY_RIGHTCTRL        97
#define KEY_KPSLASH          98
#define KEY_SYSRQ            99
#define KEY_RIGHTALT         100
#define KEY_HOME             102
#define KEY_UP               103
#define KEY_PAGEUP           104
#define KEY_LEFT             105
#define KEY_RIGHT            106
#define KEY_END              107
#define KEY_DOWN             108
#define KEY_PAGEDOWN         109
#define KEY_INSERT           110
#define KEY_DELETE           111
#define KEY_MUTE             113
#define KEY_VOLUMEDOWN       114
#define KEY_VOLUMEUP         115
#define KEY_POWER            116
#define KEY_KPEQUAL          117
#define KEY_PAUSE            119
#define KEY_SCALE            120
#define KEY_KPCOMMA          121
#define KEY_HANGEUL          122
#define KEY_HANJA            123
#define KEY_YEN              124
#define KEY_LEFTMETA         125
#define KEY_RIGHTMETA        126
#define KEY_COMPOSE          127
#define KEY_STOP             128
#define KEY_AGAIN            129
#define KEY_PROPS            130
#define KEY_UNDO             131
#define KEY_FRONT            132
#define KEY_COPY             133
#define KEY_OPEN             134
#define KEY_PASTE            135
#define KEY_FIND             136
#define KEY_CUT              137
#define KEY_HELP             138
#define KEY_CALC             140
#define KEY_SLEEP            142
#define KEY_WAKEUP           143
#define KEY_WWW              150
#define KEY_COFFEE           152
#define KEY_BACK             158
#define KEY_FORWARD          159
#define KEY_EJECTCD          161
#define KEY_NEXTSONG         163
#define KEY_PLAYPAUSE        164
#define KEY_PREVIOUSSONG     165
#define KEY_STOPCD           166
#define KEY_REFRESH          173
#define KEY_EDIT             176
#define KEY_SCROLLUP         177
#define KEY_SCROLLDOWN       178
#define KEY_F13              183
#define KEY_F14              184
#define KEY_F15              185
#define KEY_F16              186
#define KEY_F17              187
#define KEY_F18              188
#define KEY_F19              189
#define KEY_F20              190
#define KEY_F21              191
#define KEY_F22              192
#define KEY_F23              193
#define KEY_F24              194
#define KEY_DASHBOARD        204
#define KEY_BRIGHTNESSDOWN   224
#define KEY_BRIGHTNESSUP     225
#define KEY_KBDILLUMDOWN     229
#define KEY_KBDILLUMUP       230

/* HID keyboard-page (0x07) usage -> evdev keycode. */
static const unsigned char hidToEvdev[256] = {
	/* 0x00 - 0x07 */ 0, 0, 0, 0, KEY_A, KEY_B, KEY_C, KEY_D,
	/* 0x08 - 0x0f */ KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L,
	/* 0x10 - 0x17 */ KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
	/* 0x18 - 0x1f */ KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z, KEY_1, KEY_2,
	/* 0x20 - 0x27 */ KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	/* 0x28 - 0x2f */ KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE, KEY_MINUS, KEY_EQUAL, KEY_LEFTBRACE,
	/* 0x30 - 0x37 */ KEY_RIGHTBRACE, KEY_BACKSLASH, KEY_BACKSLASH, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_DOT,
	/* 0x38 - 0x3f */ KEY_SLASH, KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
	/* 0x40 - 0x47 */ KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_SYSRQ, KEY_SCROLLLOCK,
	/* 0x48 - 0x4f */ KEY_PAUSE, KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END, KEY_PAGEDOWN, KEY_RIGHT,
	/* 0x50 - 0x57 */ KEY_LEFT, KEY_DOWN, KEY_UP, KEY_NUMLOCK, KEY_KPSLASH, KEY_KPASTERISK, KEY_KPMINUS, KEY_KPPLUS,
	/* 0x58 - 0x5f */ KEY_KPENTER, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP7,
	/* 0x60 - 0x67 */ KEY_KP8, KEY_KP9, KEY_KP0, KEY_KPDOT, KEY_102ND, KEY_COMPOSE, KEY_POWER, KEY_KPEQUAL,
	/* 0x68 - 0x6f */ KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18, KEY_F19, KEY_F20,
	/* 0x70 - 0x77 */ KEY_F21, KEY_F22, KEY_F23, KEY_F24, KEY_OPEN, KEY_HELP, KEY_PROPS, KEY_FRONT,
	/* 0x78 - 0x7f */ KEY_STOP, KEY_AGAIN, KEY_UNDO, KEY_CUT, KEY_COPY, KEY_PASTE, KEY_FIND, KEY_MUTE,
	/* 0x80 - 0x87 */ KEY_VOLUMEUP, KEY_VOLUMEDOWN, 0, 0, 0, KEY_KPCOMMA, 0, KEY_RO,
	/* 0x88 - 0x8f */ KEY_KATAKANAHIRAGANA, KEY_YEN, KEY_HENKAN, KEY_MUHENKAN, KEY_KPJPCOMMA, 0, 0, 0,
	/* 0x90 - 0x97 */ KEY_HANGEUL, KEY_HANJA, KEY_KATAKANA, KEY_HIRAGANA, KEY_ZENKAKUHANKAKU, 0, 0, 0,
	/* 0x98 - 0x9f */ 0, 0, 0, 0, 0, 0, 0, 0,
	/* 0xa0 - 0xa7 */ 0, 0, 0, 0, 0, 0, 0, 0,
	/* 0xa8 - 0xaf */ 0, 0, 0, 0, 0, 0, 0, 0,
	/* 0xb0 - 0xb7 */ 0, 0, 0, 0, 0, 0, 0, 0,
	/* 0xb8 - 0xbf */ 0, 0, 0, 0, 0, 0, 0, 0,
	/* 0xc0 - 0xc7 */ KEY_BRIGHTNESSDOWN, KEY_BRIGHTNESSUP, KEY_SCALE, KEY_DASHBOARD, KEY_KBDILLUMDOWN, KEY_KBDILLUMUP, 0, 0,
	/* 0xc8 - 0xcf */ 0, 0, 0, 0, 0, 0, 0, 0,
	/* 0xd0 - 0xd7 */ 0, 0, 0, 0, 0, 0, 0, 0,
	/* 0xd8 - 0xdf */ 0, 0, 0, 0, 0, 0, 0, 0,
	/* 0xe0 - 0xe7 */ KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT, KEY_LEFTMETA, KEY_RIGHTCTRL, KEY_RIGHTSHIFT, KEY_RIGHTALT, KEY_RIGHTMETA,
	/* 0xe8 - 0xef */ KEY_PLAYPAUSE, KEY_STOPCD, KEY_PREVIOUSSONG, KEY_NEXTSONG, KEY_EJECTCD, KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE,
	/* 0xf0 - 0xf7 */ KEY_WWW, KEY_BACK, KEY_FORWARD, KEY_STOP, KEY_FIND, KEY_SCROLLUP, KEY_SCROLLDOWN, KEY_EDIT,
	/* 0xf8 - 0xff */ KEY_SLEEP, KEY_COFFEE, KEY_REFRESH, KEY_CALC, 0, 0, 0, KEY_WAKEUP,
};

/* HID modifier bits (report byte[0], bit order LCtrl,LShift,LAlt,LGUI,
 * RCtrl,RShift,RAlt,RGUI) -> evdev keycodes; usages 0xe0-0xe7 above. */
static const unsigned char hidModEvdev[8] = {
	KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT, KEY_LEFTMETA,
	KEY_RIGHTCTRL, KEY_RIGHTSHIFT, KEY_RIGHTALT, KEY_RIGHTMETA
};

#endif /* _FBDEV_HID_EVDEV_MAP_H_ */
