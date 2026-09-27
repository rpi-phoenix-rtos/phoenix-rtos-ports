/*
 * Phoenix-RTOS
 *
 * weston-drm: the modules linked into the static weston binary, and its baked
 * default keymap.
 *
 * weston_load_module() consults weston_builtin_modules[] before dlopen()
 * (weston patch 0001): the DRM backend, the GL renderer and the kiosk shell are
 * found by the file names Weston asks for, so weston.ini, the --backend/--shell
 * options and the frontend work unchanged. A module missing here fails as an
 * unloadable module would ("Failed to load module").
 *
 * weston_builtin_xkb_keymap (weston patch 0003) is evdev/pc105/us compiled on the
 * build host (build-out/weston_keymap.h): Phoenix has no xkeyboard-config.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stddef.h>

struct weston_compositor;

/* The module entry points (declared by their modules' private headers). */
extern int weston_backend_init(struct weston_compositor *compositor, void *config_base);
extern int wet_shell_init(struct weston_compositor *ec, int *argc, char *argv[]);
extern char gl_renderer_interface; /* a struct: only its address is taken */

const struct weston_builtin_module {
	const char *name;
	const char *entrypoint;
	void *init;
} weston_builtin_modules[] = {
	{ "drm-backend.so", "weston_backend_init", (void *)weston_backend_init },
	{ "gl-renderer.so", "gl_renderer_interface", (void *)&gl_renderer_interface },
	{ "kiosk-shell.so", "wet_shell_init", (void *)wet_shell_init },
	{ NULL, NULL, NULL },
};

#include "weston_keymap.h"
