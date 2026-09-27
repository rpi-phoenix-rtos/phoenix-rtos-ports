/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm shim: GLib API newer than the ports GLib 2.56 that labwc calls.
 *
 *   g_string_replace() (GLib 2.68): replace up to `limit` (0 = all) non-overlapping
 *   occurrences of `find` in `string` by `replace`, scanning left to right; returns
 *   the number replaced. An empty `find` matches at every position once (between
 *   the characters and at both ends), as in GLib.
 */

#include <string.h>
#include <glib.h>

guint g_string_replace(GString *string, const gchar *find, const gchar *replace, guint limit);

guint g_string_replace(GString *string, const gchar *find, const gchar *replace, guint limit)
{
	gsize f_len, r_len, pos = 0;
	guint n = 0;
	const gchar *cur, *next;

	g_return_val_if_fail(string != NULL, 0);
	g_return_val_if_fail(find != NULL, 0);
	g_return_val_if_fail(replace != NULL, 0);

	f_len = strlen(find);
	r_len = strlen(replace);
	while (pos <= string->len) {
		cur = string->str + pos;
		next = (f_len == 0u) ? cur : strstr(cur, find);
		if (next == NULL) {
			break;
		}
		pos = (gsize)(next - string->str);
		g_string_erase(string, (gssize)pos, (gssize)f_len);
		g_string_insert_len(string, (gssize)pos, replace, (gssize)r_len);
		pos += r_len;
		n++;
		if (f_len == 0u) {
			pos++; /* past the next character (or the end) */
		}
		if ((limit != 0u) && (n == limit)) {
			break;
		}
	}
	return n;
}
