/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm shim: the part of HarfBuzz's GLib integration pango 1.44 uses. The
 * ports HarfBuzz is built without GLib (hb-glib.h absent); pango needs only the
 * script-code conversions, which are ISO 15924 round trips (src/hb_glib_phoenix.c).
 * HarfBuzz's own Unicode functions stay the default.
 */
#ifndef LWPHX_HB_GLIB_H
#define LWPHX_HB_GLIB_H

#include <glib.h>
#include <hb.h>

HB_BEGIN_DECLS

hb_script_t hb_glib_script_to_script(GUnicodeScript script);
GUnicodeScript hb_glib_script_from_script(hb_script_t script);

HB_END_DECLS

#endif
