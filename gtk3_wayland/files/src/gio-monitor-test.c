/*
 * gio-monitor-test: checks that GIO file monitoring works.
 *
 *   gio-monitor-test [DIR]      self-test in a new directory under DIR
 *                               (default: the GLib tmp dir); exit 0 on PASS
 *   gio-monitor-test -w PATH... print every event on PATH (a directory or
 *                               a file) until interrupted
 *
 * The self-test monitors a directory and a single file, changes them from the
 * same process (create, append, chmod, rename, mkdir, delete) and waits up to
 * WAIT_SECS for each expected event, printing how long each one took.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#define WAIT_SECS 15

typedef struct {
	GFileMonitorEvent event;
	const char *tag; /* which monitor */
	char *name;  /* basename of the event's file */
	char *other; /* basename of the other file, for RENAMED */
} Event;

static GQueue events = G_QUEUE_INIT;
static gboolean verbose = TRUE;
static int appinfo_changes;


static const char *event_name(GFileMonitorEvent event)
{
	switch (event) {
		case G_FILE_MONITOR_EVENT_CHANGED: return "CHANGED";
		case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT: return "CHANGES_DONE_HINT";
		case G_FILE_MONITOR_EVENT_DELETED: return "DELETED";
		case G_FILE_MONITOR_EVENT_CREATED: return "CREATED";
		case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED: return "ATTRIBUTE_CHANGED";
		case G_FILE_MONITOR_EVENT_PRE_UNMOUNT: return "PRE_UNMOUNT";
		case G_FILE_MONITOR_EVENT_UNMOUNTED: return "UNMOUNTED";
		case G_FILE_MONITOR_EVENT_MOVED: return "MOVED";
		case G_FILE_MONITOR_EVENT_RENAMED: return "RENAMED";
		case G_FILE_MONITOR_EVENT_MOVED_IN: return "MOVED_IN";
		case G_FILE_MONITOR_EVENT_MOVED_OUT: return "MOVED_OUT";
		default: return "?";
	}
}


static void on_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
	const char *tag = data;
	(void)monitor;
	char *path = g_file_get_path(file);
	char *opath = (other != NULL) ? g_file_get_path(other) : NULL;

	if (verbose) {
		printf("  [%s] %-18s %s%s%s\n", tag, event_name(event), path, (opath != NULL) ? " -> " : "",
			(opath != NULL) ? opath : "");
		fflush(stdout);
	}

	Event *e = g_new0(Event, 1);
	e->event = event;
	e->tag = tag;
	e->name = g_file_get_basename(file);
	e->other = (other != NULL) ? g_file_get_basename(other) : NULL;
	g_queue_push_tail(&events, e);

	g_free(path);
	g_free(opath);
}


static void event_free(gpointer p)
{
	Event *e = p;
	g_free(e->name);
	g_free(e->other);
	g_free(e);
}


/* Run the main loop until @event on @name (and @other) arrives from the monitor
 * @tag; the events before it are dropped. */
static gboolean wait_for(const char *tag, GFileMonitorEvent event, const char *name, const char *other, const char *what)
{
	gint64 start = g_get_monotonic_time();
	gint64 deadline = start + (gint64)WAIT_SECS * G_USEC_PER_SEC;

	for (;;) {
		Event *e;
		while ((e = g_queue_pop_head(&events)) != NULL) {
			gboolean match = (strcmp(e->tag, tag) == 0) && (e->event == event) && (strcmp(e->name, name) == 0) &&
				((other == NULL) || ((e->other != NULL) && (strcmp(e->other, other) == 0)));
			event_free(e);
			if (match) {
				printf("PASS %-40s %s after %.2f s\n", what, event_name(event),
					(g_get_monotonic_time() - start) / 1e6);
				return TRUE;
			}
		}
		if (g_get_monotonic_time() >= deadline) {
			printf("FAIL %-40s no %s on %s within %d s\n", what, event_name(event), name, WAIT_SECS);
			return FALSE;
		}
		g_main_context_iteration(NULL, FALSE);
		g_usleep(10000);
	}
}


static void on_appinfo_changed(GAppInfoMonitor *m, gpointer data)
{
	(void)m;
	(void)data;
	appinfo_changes++;
	printf("  [appinfo] changed\n");
}


/* GDesktopAppInfo watches the applications directories with monitors that run
 * on GLib's worker thread (the panel, the app finder and menus rely on it). */
static gboolean wait_for_appinfo(const char *what)
{
	gint64 start = g_get_monotonic_time();
	gint64 deadline = start + (gint64)WAIT_SECS * G_USEC_PER_SEC;

	while (appinfo_changes == 0) {
		if (g_get_monotonic_time() >= deadline) {
			printf("FAIL %-40s no GAppInfoMonitor::changed within %d s\n", what, WAIT_SECS);
			return FALSE;
		}
		g_main_context_iteration(NULL, FALSE);
		g_usleep(10000);
	}
	printf("PASS %-40s GAppInfoMonitor::changed after %.2f s\n", what, (g_get_monotonic_time() - start) / 1e6);
	appinfo_changes = 0;
	return TRUE;
}


static int write_file(const char *path, const char *text, gboolean append)
{
	int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
	if (fd < 0) {
		return -1;
	}
	ssize_t n = write(fd, text, strlen(text));
	close(fd);
	return (n == (ssize_t)strlen(text)) ? 0 : -1;
}


/* Let events of the previous step drain and the poller take its snapshot. */
static void settle(void)
{
	gint64 until = g_get_monotonic_time() + 200000;
	while (g_get_monotonic_time() < until) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(10000);
	}
	g_queue_clear_full(&events, event_free);
}


static GFileMonitor *monitor(const char *path, gboolean dir, GFileMonitorFlags flags, const char *tag)
{
	GError *err = NULL;
	GFile *f = g_file_new_for_path(path);
	GFileMonitor *m = dir ? g_file_monitor_directory(f, flags, NULL, &err) : g_file_monitor_file(f, flags, NULL, &err);
	g_object_unref(f);
	if (m == NULL) {
		printf("FAIL monitor %s: %s\n", path, err->message);
		g_error_free(err);
		return NULL;
	}
	printf("monitor %s (%s): %s\n", path, dir ? "directory" : "file", G_OBJECT_TYPE_NAME(m));
	g_signal_connect(m, "changed", G_CALLBACK(on_changed), (gpointer)tag);
	return m;
}


static int self_test(const char *base)
{
	int fails = 0;
	char *tmpl = g_build_filename(base, "gio-monitor-XXXXXX", NULL);
	char *dir = g_mkdtemp(tmpl);
	if (dir == NULL) {
		printf("FAIL mkdtemp %s: %s\n", tmpl, strerror(errno));
		return 1;
	}

	/* before any other GIO call: the applications directory GDesktopAppInfo watches */
	char *apps = g_build_filename(dir, "xdg", "applications", NULL);
	char *desktop = g_build_filename(apps, "gio-monitor-test.desktop", NULL);
	g_mkdir_with_parents(apps, 0755);
	char *xdg = g_path_get_dirname(apps);
	g_setenv("XDG_DATA_HOME", xdg, TRUE);

	char *a = g_build_filename(dir, "a", NULL);
	char *b = g_build_filename(dir, "b", NULL);
	char *sub = g_build_filename(dir, "sub", NULL);
	char *f = g_build_filename(dir, "watched-file", NULL);

	GFileMonitor *dm = monitor(dir, TRUE, G_FILE_MONITOR_WATCH_MOVES, "dir");
	GFileMonitor *fm = monitor(f, FALSE, G_FILE_MONITOR_NONE, "file");
	if ((dm == NULL) || (fm == NULL)) {
		return 1;
	}
	settle();

	/* directory monitor */
	write_file(a, "hello\n", FALSE);
	fails += !wait_for("dir", G_FILE_MONITOR_EVENT_CREATED, "a", NULL, "dir: create a");
	settle();
	write_file(a, "more\n", TRUE);
	fails += !wait_for("dir", G_FILE_MONITOR_EVENT_CHANGED, "a", NULL, "dir: append to a");
	settle();
	if (chmod(a, 0600) == 0) {
		fails += !wait_for("dir", G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED, "a", NULL, "dir: chmod a");
	}
	else {
		printf("SKIP %-40s chmod: %s\n", "dir: chmod a", strerror(errno));
	}
	settle();
	rename(a, b);
	fails += !wait_for("dir", G_FILE_MONITOR_EVENT_RENAMED, "a", "b", "dir: rename a -> b");
	settle();
	mkdir(sub, 0755);
	fails += !wait_for("dir", G_FILE_MONITOR_EVENT_CREATED, "sub", NULL, "dir: mkdir sub");
	settle();
	unlink(b);
	fails += !wait_for("dir", G_FILE_MONITOR_EVENT_DELETED, "b", NULL, "dir: unlink b");
	settle();
	rmdir(sub);
	fails += !wait_for("dir", G_FILE_MONITOR_EVENT_DELETED, "sub", NULL, "dir: rmdir sub");
	settle();

	/* file monitor (the directory monitor sees these too) */
	write_file(f, "1\n", FALSE);
	fails += !wait_for("file", G_FILE_MONITOR_EVENT_CREATED, "watched-file", NULL, "file: create");
	settle();
	write_file(f, "22\n", TRUE);
	fails += !wait_for("file", G_FILE_MONITOR_EVENT_CHANGED, "watched-file", NULL, "file: append");
	settle();
	unlink(f);
	fails += !wait_for("file", G_FILE_MONITOR_EVENT_DELETED, "watched-file", NULL, "file: unlink");

	/* GDesktopAppInfo (monitors on the worker thread) */
	GAppInfoMonitor *am = g_app_info_monitor_get();
	g_signal_connect(am, "changed", G_CALLBACK(on_appinfo_changed), NULL);
	GList *all = g_app_info_get_all(); /* loads the directories and starts watching them */
	g_list_free_full(all, g_object_unref);
	settle();
	appinfo_changes = 0;
	write_file(desktop, "[Desktop Entry]\nType=Application\nName=gio-monitor-test\nExec=true\n", FALSE);
	if (wait_for_appinfo("appinfo: new .desktop file")) {
		GDesktopAppInfo *info = g_desktop_app_info_new("gio-monitor-test.desktop");
		printf("%s %-40s %s\n", (info != NULL) ? "PASS" : "FAIL", "appinfo: lookup new entry",
			(info != NULL) ? g_app_info_get_name(G_APP_INFO(info)) : "not found");
		fails += (info == NULL);
		g_clear_object(&info);
	}
	else {
		fails++;
	}
	unlink(desktop);
	fails += !wait_for_appinfo("appinfo: .desktop file removed");
	g_object_unref(am);
	rmdir(apps);
	rmdir(xdg);

	g_file_monitor_cancel(dm);
	g_file_monitor_cancel(fm);
	g_object_unref(dm);
	g_object_unref(fm);
	rmdir(dir);

	printf("%s: %d failure(s)\n", (fails == 0) ? "PASS" : "FAIL", fails);
	return (fails == 0) ? 0 : 1;
}


static int watch(int argc, char **argv)
{
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	int n = 0;

	for (int i = 0; i < argc; i++) {
		struct stat st;
		gboolean dir = (stat(argv[i], &st) == 0) && S_ISDIR(st.st_mode);
		n += (monitor(argv[i], dir, G_FILE_MONITOR_WATCH_MOVES, argv[i]) != NULL);
	}
	if (n == 0) {
		return 1;
	}
	g_main_loop_run(loop);
	return 0;
}


int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	if ((argc >= 2) && (strcmp(argv[1], "-w") == 0)) {
		return watch(argc - 2, argv + 2);
	}
	if ((argc >= 2) && (argv[1][0] == '-')) {
		fprintf(stderr, "usage: %s [DIR] | -w PATH...\n", argv[0]);
		return 2;
	}
	return self_test((argc >= 2) ? argv[1] : g_get_tmp_dir());
}
