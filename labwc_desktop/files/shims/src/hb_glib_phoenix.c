/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm shim: hb_glib_script_to_script() / hb_glib_script_from_script() (see
 * include/hb-glib.h). Both libraries map their script enums to the same ISO 15924
 * four-letter tags, so the conversion is a round trip through the tag -- what
 * HarfBuzz's own hb-glib.cc does for scripts newer than its table.
 */

#include <hb-glib.h>

hb_script_t hb_glib_script_to_script(GUnicodeScript script)
{
	return hb_script_from_iso15924_tag((hb_tag_t)g_unicode_script_to_iso15924(script));
}


GUnicodeScript hb_glib_script_from_script(hb_script_t script)
{
	return g_unicode_script_from_iso15924((guint32)hb_script_to_iso15924_tag(script));
}
