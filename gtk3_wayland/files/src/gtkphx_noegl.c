/*
 * gtk3-wayland: "no EGL" for GTK 3 programs linked without Mesa.
 *
 * libepoxy (the xorg-drm build, static-EGL dispatch patch) resolves every EGL/GL
 * entry point through the linked implementation's eglGetProcAddress(). GTK 3 needs
 * EGL only for GL contexts (GtkGLArea, GDK_GL=always), which it creates lazily; a
 * plain widget program never gets there. This eglGetProcAddress() answers the few
 * calls GDK's Wayland GL initialisation makes before it gives up
 * (gdk_wayland_display_init_gl: eglQueryString, eglGetPlatformDisplay(EXT),
 * eglGetDisplay) so that path ends in GDK's own "No GL implementation is
 * available" error instead of an epoxy abort. Everything else is NULL.
 *
 * A program that wants GL links the Mesa --wayland EGL closure instead of this
 * archive (it defines the real eglGetProcAddress).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stddef.h>
#include <string.h>

typedef void (*gtkphx_proc)(void);

#define GTKPHX_EGL_NOT_INITIALIZED 0x3001
#define GTKPHX_EGL_FALSE 0u

static const char *noegl_query_string(void *dpy, int name)
{
	(void)dpy;
	(void)name;
	return NULL;
}

static void *noegl_get_display(void *native)
{
	(void)native;
	return NULL;
}

static void *noegl_get_platform_display(unsigned int platform, void *native, const void *attribs)
{
	(void)platform;
	(void)native;
	(void)attribs;
	return NULL;
}

static unsigned int noegl_initialize(void *dpy, int *major, int *minor)
{
	(void)dpy;
	(void)major;
	(void)minor;
	return GTKPHX_EGL_FALSE;
}

static int noegl_get_error(void)
{
	return GTKPHX_EGL_NOT_INITIALIZED;
}

gtkphx_proc eglGetProcAddress(const char *name)
{
	if (name == NULL)
		return NULL;
	if (strcmp(name, "eglQueryString") == 0)
		return (gtkphx_proc)noegl_query_string;
	if (strcmp(name, "eglGetDisplay") == 0)
		return (gtkphx_proc)noegl_get_display;
	if (strcmp(name, "eglGetPlatformDisplay") == 0 || strcmp(name, "eglGetPlatformDisplayEXT") == 0)
		return (gtkphx_proc)noegl_get_platform_display;
	if (strcmp(name, "eglInitialize") == 0)
		return (gtkphx_proc)noegl_initialize;
	if (strcmp(name, "eglGetError") == 0)
		return (gtkphx_proc)noegl_get_error;
	return NULL;
}
