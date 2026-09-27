/*
 * Phoenix-RTOS
 *
 * gtk3-hello: the smallest useful GTK 3 test for the Wayland port (M7).
 *
 * A GtkWindow with a GtkLabel, a GtkButton and a GtkTreeView listing the entries
 * of a directory through GIO (the local GFile backend, no gvfs). It prints tagged
 * lines on stdout ("GTK3HELLO ...") at each stage, clicks its own button once, counts
 * frame-clock ticks (Wayland frame callbacks) and quits by itself after --seconds.
 *
 *   gtk3-hello [--seconds N] [--dir PATH] [--no-click] [--layer]
 *
 * --layer makes the window a layer-shell surface (gtk-layer-shell: a top-anchored bar,
 * as xfce4-panel is) when the compositor offers zwlr_layer_shell_v1 (labwc does, Weston's
 * kiosk shell does not: the window then stays a normal toplevel).
 *
 * Copyright 2026 Phoenix Systems
 * %LICENSE%
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <gtk-layer-shell.h>

static struct {
	GtkWidget *window;
	GtkWidget *label;
	GtkWidget *button;
	GtkListStore *store;
	int clicks;
	unsigned int ticks;
	unsigned int draws;
	gint64 t0;
	int seconds;
	int click;
	int layer;
	const char *dir;
	int rc;
} hello = { .seconds = 20, .click = 1, .dir = "/", .rc = 0 };

static double elapsed_s(void)
{
	return (double)(g_get_monotonic_time() - hello.t0) / 1e6;
}

static void on_clicked(GtkButton *button, gpointer data)
{
	char text[96];

	(void)button;
	(void)data;
	hello.clicks++;
	snprintf(text, sizeof(text), "Hello from GTK %u.%u.%u on Phoenix-RTOS \342\200\224 clicked %d time%s",
		gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version(), hello.clicks,
		hello.clicks == 1 ? "" : "s");
	gtk_label_set_text(GTK_LABEL(hello.label), text);
	printf("GTK3HELLO clicked n=%d t=%.2f\n", hello.clicks, elapsed_s());
	fflush(stdout);
}

static int fill_store(const char *path)
{
	GFile *dir = g_file_new_for_path(path);
	GError *err = NULL;
	GFileEnumerator *en;
	GFileInfo *info;
	GtkTreeIter it;
	int n = 0;

	en = g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE ","
		G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &err);
	if (en == NULL) {
		printf("GTK3HELLO gio dir=%s rc=error msg=\"%s\"\n", path, err != NULL ? err->message : "?");
		g_clear_error(&err);
		g_object_unref(dir);
		return -1;
	}
	while ((info = g_file_enumerator_next_file(en, NULL, &err)) != NULL) {
		GFileType type = g_file_info_get_file_type(info);
		const char *kind = type == G_FILE_TYPE_DIRECTORY ? "dir" : type == G_FILE_TYPE_SYMBOLIC_LINK ? "link" : "file";

		gtk_list_store_append(hello.store, &it);
		gtk_list_store_set(hello.store, &it, 0, g_file_info_get_name(info), 1, kind,
			2, (guint64)g_file_info_get_size(info), -1);
		n++;
		g_object_unref(info);
	}
	if (err != NULL) {
		printf("GTK3HELLO gio dir=%s next=error msg=\"%s\"\n", path, err->message);
		g_clear_error(&err);
	}
	g_object_unref(en);
	g_object_unref(dir);
	printf("GTK3HELLO gio dir=%s rc=0 entries=%d\n", path, n);
	return n;
}

static gboolean on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	(void)w;
	(void)clock;
	(void)data;
	hello.ticks++;
	return G_SOURCE_CONTINUE;
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	(void)w;
	(void)cr;
	(void)data;
	if (hello.draws++ == 0) {
		printf("GTK3HELLO first-draw t=%.2f\n", elapsed_s());
		fflush(stdout);
	}
	return FALSE;
}

static gboolean on_map(GtkWidget *w, GdkEvent *ev, gpointer data)
{
	int width, height;

	(void)ev;
	(void)data;
	gtk_window_get_size(GTK_WINDOW(w), &width, &height);
	printf("GTK3HELLO mapped t=%.2f size=%dx%d scale=%d\n", elapsed_s(), width, height, gtk_widget_get_scale_factor(w));
	fflush(stdout);
	return FALSE;
}

static gboolean do_click(gpointer data)
{
	(void)data;
	gtk_button_clicked(GTK_BUTTON(hello.button));
	return G_SOURCE_REMOVE;
}

static gboolean heartbeat(gpointer data)
{
	(void)data;
	printf("GTK3HELLO hold t=%.1f ticks=%u draws=%u clicks=%d\n", elapsed_s(), hello.ticks, hello.draws, hello.clicks);
	fflush(stdout);
	return G_SOURCE_CONTINUE;
}

static gboolean do_quit(gpointer data)
{
	(void)data;
	printf("GTK3HELLO quit t=%.2f ticks=%u draws=%u clicks=%d\n", elapsed_s(), hello.ticks, hello.draws, hello.clicks);
	fflush(stdout);
	gtk_main_quit();
	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	GtkWidget *box, *scroll, *tree;
	GtkCellRenderer *r;
	GdkDisplay *display;
	int i, n;

	hello.t0 = g_get_monotonic_time();
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
			hello.seconds = atoi(argv[++i]);
		else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc)
			hello.dir = argv[++i];
		else if (strcmp(argv[i], "--no-click") == 0)
			hello.click = 0;
		else if (strcmp(argv[i], "--layer") == 0)
			hello.layer = 1;
	}
	printf("GTK3HELLO start gtk=%u.%u.%u glib=%u.%u.%u GDK_BACKEND=%s WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s\n",
		gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version(), glib_major_version,
		glib_minor_version, glib_micro_version, g_getenv("GDK_BACKEND") ? g_getenv("GDK_BACKEND") : "(unset)",
		g_getenv("WAYLAND_DISPLAY") ? g_getenv("WAYLAND_DISPLAY") : "(unset)",
		g_getenv("XDG_RUNTIME_DIR") ? g_getenv("XDG_RUNTIME_DIR") : "(unset)");
	fflush(stdout);

	if (!gtk_init_check(&argc, &argv)) {
		printf("GTK3HELLO init=failed (no display) t=%.2f\n", elapsed_s());
		return 2;
	}
	display = gdk_display_get_default();
	printf("GTK3HELLO init=ok t=%.2f display=%s backend=%s\n", elapsed_s(), gdk_display_get_name(display),
		GDK_IS_WAYLAND_DISPLAY(display) ? "wayland" : "other");
	fflush(stdout);

	hello.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(hello.window), "gtk3-hello");
	gtk_window_set_default_size(GTK_WINDOW(hello.window), 640, 480);
	g_signal_connect(hello.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(hello.window, "map-event", G_CALLBACK(on_map), NULL);
	if (hello.layer) {
		if (gtk_layer_is_supported()) {
			gtk_layer_init_for_window(GTK_WINDOW(hello.window));
			gtk_layer_set_layer(GTK_WINDOW(hello.window), GTK_LAYER_SHELL_LAYER_TOP);
			gtk_layer_set_anchor(GTK_WINDOW(hello.window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
			gtk_layer_set_anchor(GTK_WINDOW(hello.window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
			gtk_layer_set_anchor(GTK_WINDOW(hello.window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
			gtk_layer_auto_exclusive_zone_enable(GTK_WINDOW(hello.window));
		}
		printf("GTK3HELLO layer=%s protocol_version=%u\n", gtk_layer_is_supported() ? "supported" : "unsupported",
			gtk_layer_get_protocol_version());
		fflush(stdout);
	}

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	gtk_container_add(GTK_CONTAINER(hello.window), box);

	hello.label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(hello.label), "<big><b>Hello from GTK 3 on Phoenix-RTOS</b></big>");
	gtk_box_pack_start(GTK_BOX(box), hello.label, FALSE, FALSE, 0);
	g_signal_connect(hello.label, "draw", G_CALLBACK(on_draw), NULL);

	hello.button = gtk_button_new_with_label("Click me");
	g_signal_connect(hello.button, "clicked", G_CALLBACK(on_clicked), NULL);
	gtk_box_pack_start(GTK_BOX(box), hello.button, FALSE, FALSE, 0);

	hello.store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64);
	n = fill_store(hello.dir);
	tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(hello.store));
	r = gtk_cell_renderer_text_new();
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "Name", r, "text", 0, NULL);
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "Kind", r, "text", 1, NULL);
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "Size", r, "text", 2, NULL);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_container_add(GTK_CONTAINER(scroll), tree);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

	gtk_widget_add_tick_callback(hello.window, on_tick, NULL, NULL);
	gtk_widget_show_all(hello.window);
	printf("GTK3HELLO shown t=%.2f rows=%d\n", elapsed_s(), n);
	fflush(stdout);

	if (hello.click)
		g_timeout_add(3000, do_click, NULL);
	g_timeout_add_seconds(5, heartbeat, NULL);
	if (hello.seconds > 0)
		g_timeout_add_seconds(hello.seconds, do_quit, NULL);

	gtk_main();
	printf("GTK3HELLO exit rc=%d t=%.2f\n", hello.rc, elapsed_s());
	fflush(stdout);
	return hello.rc;
}
