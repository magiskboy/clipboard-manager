#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include <errno.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

static void (*g_on_changed)(const char *utf8, const char *source_app, void *ud);
static void *g_userdata;
static char *g_last_snapshot;

#ifdef GDK_WINDOWING_X11
static char *linux_read_proc_comm(unsigned long pid_ul)
{
	char path[64];
	char line[256];
	FILE *f;
	pid_t pid;

	pid = (pid_t)pid_ul;
	if (pid <= 0)
		return NULL;
	snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
	f = fopen(path, "r");
	if (f == NULL)
		return NULL;
	if (fgets(line, sizeof line, f) == NULL) {
		fclose(f);
		return NULL;
	}
	fclose(f);
	line[strcspn(line, "\n")] = '\0';
	if (line[0] == '\0')
		return NULL;
	return strdup(line);
}

static int x11_get_net_wm_pid(Display *dpy, Window w, unsigned long *pid_out)
{
	Atom xa_pid;
	Atom act_type;
	int act_fmt;
	unsigned long nitems;
	unsigned long bytes_after;
	unsigned char *prop = NULL;
	int r;

	xa_pid = XInternAtom(dpy, "_NET_WM_PID", False);
	if (XGetWindowProperty(dpy, w, xa_pid, 0, 1, False, XA_CARDINAL,
		&act_type, &act_fmt, &nitems, &bytes_after, &prop) != Success)
		return -1;
	if (act_type != XA_CARDINAL || nitems < 1 || prop == NULL) {
		if (prop != NULL)
			XFree(prop);
		return -1;
	}
	*pid_out = *(unsigned long *)prop;
	XFree(prop);
	r = 0;
	(void)act_fmt;
	(void)bytes_after;
	return r;
}

static char *x11_resolve_window_app(Display *dpy, Window w)
{
	Window root_ret;
	Window parent;
	Window *children = NULL;
	unsigned int nchild;
	unsigned long pid;
	char *s;
	XClassHint ch;

	for (;;) {
		if (x11_get_net_wm_pid(dpy, w, &pid) == 0) {
			s = linux_read_proc_comm(pid);
			if (s != NULL)
				return s;
		}
		if (XGetClassHint(dpy, w, &ch)) {
			s = NULL;
			if (ch.res_name != NULL && ch.res_name[0] != '\0')
				s = strdup(ch.res_name);
			else if (ch.res_class != NULL && ch.res_class[0] != '\0')
				s = strdup(ch.res_class);
			if (ch.res_name != NULL)
				XFree(ch.res_name);
			if (ch.res_class != NULL)
				XFree(ch.res_class);
			if (s != NULL)
				return s;
		}
		if (!XQueryTree(dpy, w, &root_ret, &parent, &children, &nchild))
			return NULL;
		if (children != NULL) {
			XFree(children);
			children = NULL;
		}
		if (parent == None || parent == root_ret || parent == w)
			return NULL;
		w = parent;
	}
}

static char *linux_clipboard_source_app(void)
{
	GdkDisplay *gd;
	Display *dpy;
	Atom sel;
	Window owner;

	gd = gdk_display_get_default();
	if (gd == NULL || !GDK_IS_X11_DISPLAY(gd))
		return NULL;
	dpy = gdk_x11_display_get_xdisplay(gd);
	sel = XInternAtom(dpy, "CLIPBOARD", False);
	owner = XGetSelectionOwner(dpy, sel);
	if (owner == None)
		return NULL;
	return x11_resolve_window_app(dpy, owner);
}
#else
static char *linux_clipboard_source_app(void)
{
	return NULL;
}
#endif

static void clipboard_text_received(GtkClipboard *clipboard, const gchar *text,
	gpointer data)
{
	char *src;

	(void)clipboard;
	(void)data;

	if (text == NULL || text[0] == '\0')
		return;
	if (g_last_snapshot != NULL && strcmp(g_last_snapshot, text) == 0)
		return;
	free(g_last_snapshot);
	g_last_snapshot = strdup(text);
	if (g_last_snapshot == NULL)
		return;
	src = linux_clipboard_source_app();
	if (g_on_changed != NULL)
		g_on_changed(text, src, g_userdata);
	free(src);
}

void platform_clipboard_init(void (*on_changed)(const char *utf8,
	const char *source_app, void *ud), void *userdata)
{
	GtkClipboard *clip;
	gchar *initial;

	g_on_changed = on_changed;
	g_userdata = userdata;
	free(g_last_snapshot);
	g_last_snapshot = NULL;

	clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	initial = gtk_clipboard_wait_for_text(clip);
	if (initial != NULL) {
		g_last_snapshot = strdup(initial);
		g_free(initial);
	}
}

void platform_clipboard_shutdown(void)
{
	g_on_changed = NULL;
	g_userdata = NULL;
	free(g_last_snapshot);
	g_last_snapshot = NULL;
}

void platform_clipboard_poll(void)
{
	GtkClipboard *clip;

	clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	gtk_clipboard_request_text(clip, clipboard_text_received, NULL);
}

char *platform_clipboard_get_text(void)
{
	GtkClipboard *clip;
	gchar *g;
	char *out;

	clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	g = gtk_clipboard_wait_for_text(clip);
	if (g == NULL)
		return NULL;
	out = strdup(g);
	g_free(g);
	return out;
}

int platform_clipboard_set_text(const char *utf8)
{
	GtkClipboard *clip;

	if (utf8 == NULL)
		return -1;
	clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	gtk_clipboard_set_text(clip, utf8, -1);
	return 0;
}

static void clip_list_tv_size_alloc(GtkWidget *tvw, GdkRectangle *alloc,
	gpointer data)
{
	GtkTreeViewColumn *col;

	(void)data;
	col = gtk_tree_view_get_column(GTK_TREE_VIEW(tvw), 0);
	if (col == NULL || alloc->width <= 1)
		return;
	gtk_tree_view_column_set_fixed_width(col, alloc->width);
}

void platform_polish_clipboard_list(uiTable *table)
{
	GtkWidget *swidget;
	GtkScrolledWindow *sw;
	GtkWidget *tvw;
	GtkTreeViewColumn *col;
	GList *cells;
	GList *rc;

	if (table == NULL)
		return;
	swidget = GTK_WIDGET(uiControlHandle(uiControl(table)));
	if (!GTK_IS_SCROLLED_WINDOW(swidget))
		return;
	sw = GTK_SCROLLED_WINDOW(swidget);
	gtk_scrolled_window_set_policy(sw, GTK_POLICY_NEVER,
		GTK_POLICY_AUTOMATIC);
	tvw = gtk_bin_get_child(GTK_BIN(sw));
	if (tvw == NULL || !GTK_IS_TREE_VIEW(tvw))
		return;
	col = gtk_tree_view_get_column(GTK_TREE_VIEW(tvw), 0);
	if (col == NULL)
		return;
	gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
	gtk_tree_view_column_set_resizable(col, FALSE);
	gtk_tree_view_column_set_expand(col, TRUE);
	cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(col));
	for (rc = cells; rc != NULL; rc = rc->next) {
		if (GTK_IS_CELL_RENDERER_TEXT(rc->data))
			g_object_set(G_OBJECT(rc->data), "ellipsize",
				PANGO_ELLIPSIZE_END, NULL);
	}
	g_list_free(cells);
	g_signal_connect_after(tvw, "size-allocate",
		G_CALLBACK(clip_list_tv_size_alloc), NULL);
}

int platform_user_config_path(char *buf, size_t buf_size)
{
	const char *xdg;
	const char *home;

	if (buf == NULL || buf_size == 0)
		return -1;
	xdg = getenv("XDG_CONFIG_HOME");
	if (xdg != NULL && xdg[0] != '\0') {
		if (snprintf(buf, buf_size, "%s/clipboard-manager/config.ini", xdg) >=
		    (int)buf_size)
			return -1;
		return 0;
	}
	home = getenv("HOME");
	if (home != NULL && home[0] != '\0') {
		if (snprintf(buf, buf_size, "%s/.config/clipboard-manager/config.ini",
		    home) >= (int)buf_size)
			return -1;
		return 0;
	}
	return -1;
}

static int mkdir_p(char *path)
{
	char *p;
	int r;

	for (p = path + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		r = mkdir(path, 0755);
		if (r != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	r = mkdir(path, 0755);
	if (r != 0 && errno != EEXIST)
		return -1;
	return 0;
}

int platform_ensure_config_parent(const char *utf8_path)
{
	char *copy;
	char *slash;

	if (utf8_path == NULL || utf8_path[0] == '\0')
		return -1;
	copy = strdup(utf8_path);
	if (copy == NULL)
		return -1;
	slash = strrchr(copy, '/');
	if (slash == NULL || slash == copy) {
		free(copy);
		return 0;
	}
	*slash = '\0';
	if (mkdir_p(copy) != 0) {
		free(copy);
		return -1;
	}
	free(copy);
	return 0;
}

static platform_tray_show_cb s_tray_show;
static platform_tray_quit_cb s_tray_quit;
static void *s_tray_ud;
static GtkStatusIcon *s_tray_icon;
static GtkWidget *s_tray_menu;
static void *s_appind_lib;
static gpointer s_app_indicator;

typedef gpointer (*fn_app_indicator_new)(const gchar *id, const gchar *icon,
	int category);
typedef void (*fn_app_indicator_set_status)(gpointer self, int status);
typedef void (*fn_app_indicator_set_menu)(gpointer self, GtkMenu *menu);
typedef void (*fn_app_indicator_set_icon_full)(gpointer self,
	const gchar *icon_name, const gchar *icon_desc);

static fn_app_indicator_new p_app_indicator_new;
static fn_app_indicator_set_status p_app_indicator_set_status;
static fn_app_indicator_set_menu p_app_indicator_set_menu;
static fn_app_indicator_set_icon_full p_app_indicator_set_icon_full;

static int resolve_tray_icon_path(char *buf, size_t buf_size)
{
	ssize_t n;
	char *slash;

	if (buf == NULL || buf_size == 0)
		return -1;
	n = readlink("/proc/self/exe", buf, buf_size - 1);
	if (n <= 0 || (size_t)n >= buf_size)
		return -1;
	buf[n] = '\0';
	slash = strrchr(buf, '/');
	if (slash == NULL)
		return -1;
	*slash = '\0';
	if (snprintf(slash, buf_size - (size_t)(slash - buf),
	    "/../assets/app-icon.svg") >=
	    (int)(buf_size - (size_t)(slash - buf)))
		return -1;
	return 0;
}

static void tray_menu_show(GtkMenuItem *item, gpointer data)
{
	(void)item;
	(void)data;
	if (s_tray_show != NULL)
		s_tray_show(s_tray_ud);
}

static void tray_menu_quit(GtkMenuItem *item, gpointer data)
{
	(void)item;
	(void)data;
	if (s_tray_quit != NULL)
		s_tray_quit(s_tray_ud);
}

static void tray_icon_activate(GtkStatusIcon *icon, gpointer data)
{
	(void)icon;
	(void)data;
	if (s_tray_show != NULL)
		s_tray_show(s_tray_ud);
}

static void tray_icon_popup(GtkStatusIcon *icon, guint button, guint t,
	gpointer data)
{
	GtkWidget *menu;
	GtkWidget *mi;

	(void)data;
	(void)icon;
	menu = gtk_menu_new();
	mi = gtk_menu_item_new_with_label("Show window");
	g_signal_connect(mi, "activate", G_CALLBACK(tray_menu_show), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
	mi = gtk_menu_item_new_with_label("Quit");
	g_signal_connect(mi, "activate", G_CALLBACK(tray_menu_quit), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
	gtk_widget_show_all(menu);
#if GTK_CHECK_VERSION(3, 22, 0)
	{
		GdkEvent *ev;

		ev = gtk_get_current_event();
		gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
	}
#else
	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, button, t);
#endif
	(void)button;
	(void)t;
}

static GtkWidget *tray_build_menu(void)
{
	GtkWidget *menu;
	GtkWidget *mi;

	menu = gtk_menu_new();
	mi = gtk_menu_item_new_with_label("Show window");
	g_signal_connect(mi, "activate", G_CALLBACK(tray_menu_show), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
	mi = gtk_menu_item_new_with_label("Quit");
	g_signal_connect(mi, "activate", G_CALLBACK(tray_menu_quit), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
	gtk_widget_show_all(menu);
	return menu;
}

static int appindicator_try_init(const char *icon_path)
{
	static const char *libs[] = {
		"libayatana-appindicator3.so.1",
		"libappindicator3.so.1",
	};
	size_t i;

	for (i = 0; i < sizeof libs / sizeof libs[0]; i++) {
		s_appind_lib = dlopen(libs[i], RTLD_NOW);
		if (s_appind_lib != NULL)
			break;
	}
	if (s_appind_lib == NULL)
		return -1;
	p_app_indicator_new = (fn_app_indicator_new)dlsym(s_appind_lib,
		"app_indicator_new");
	p_app_indicator_set_status = (fn_app_indicator_set_status)dlsym(s_appind_lib,
		"app_indicator_set_status");
	p_app_indicator_set_menu = (fn_app_indicator_set_menu)dlsym(s_appind_lib,
		"app_indicator_set_menu");
	p_app_indicator_set_icon_full = (fn_app_indicator_set_icon_full)dlsym(
		s_appind_lib, "app_indicator_set_icon_full");
	if (p_app_indicator_new == NULL || p_app_indicator_set_status == NULL ||
	    p_app_indicator_set_menu == NULL) {
		dlclose(s_appind_lib);
		s_appind_lib = NULL;
		return -1;
	}
	s_app_indicator = p_app_indicator_new("clipboard-manager", "edit-paste", 0);
	if (s_app_indicator == NULL) {
		dlclose(s_appind_lib);
		s_appind_lib = NULL;
		return -1;
	}
	s_tray_menu = tray_build_menu();
	p_app_indicator_set_menu(s_app_indicator, GTK_MENU(s_tray_menu));
	if (icon_path != NULL && icon_path[0] != '\0' &&
	    p_app_indicator_set_icon_full != NULL)
		p_app_indicator_set_icon_full(s_app_indicator, icon_path,
			"Clipboard manager");
	p_app_indicator_set_status(s_app_indicator, 1);
	return 0;
}

void platform_tray_init(uiWindow *main_win, platform_tray_show_cb show_cb,
	platform_tray_quit_cb quit_cb, void *userdata)
{
	char icon_path[PATH_MAX];

	(void)main_win;

	platform_tray_shutdown();
	s_tray_show = show_cb;
	s_tray_quit = quit_cb;
	s_tray_ud = userdata;
	icon_path[0] = '\0';
	if (resolve_tray_icon_path(icon_path, sizeof icon_path) == 0 &&
	    appindicator_try_init(icon_path) == 0)
		return;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	s_tray_icon = NULL;
	if (icon_path[0] != '\0')
		s_tray_icon = gtk_status_icon_new_from_file(icon_path);
	if (s_tray_icon == NULL)
		s_tray_icon = gtk_status_icon_new_from_icon_name("edit-paste");
	if (s_tray_icon == NULL)
		s_tray_icon = gtk_status_icon_new_from_icon_name("gtk-paste");
	if (s_tray_icon != NULL) {
		gtk_status_icon_set_tooltip_text(s_tray_icon,
			"Clipboard manager");
		g_signal_connect(s_tray_icon, "activate",
			G_CALLBACK(tray_icon_activate), NULL);
		g_signal_connect(s_tray_icon, "popup-menu",
			G_CALLBACK(tray_icon_popup), NULL);
		gtk_status_icon_set_visible(s_tray_icon, TRUE);
	}
#pragma GCC diagnostic pop
}

void platform_tray_shutdown(void)
{
	if (s_app_indicator != NULL) {
		if (p_app_indicator_set_status != NULL)
			p_app_indicator_set_status(s_app_indicator, 0);
		g_object_unref(s_app_indicator);
		s_app_indicator = NULL;
	}
	if (s_appind_lib != NULL) {
		dlclose(s_appind_lib);
		s_appind_lib = NULL;
	}
	p_app_indicator_new = NULL;
	p_app_indicator_set_status = NULL;
	p_app_indicator_set_menu = NULL;
	p_app_indicator_set_icon_full = NULL;
	if (s_tray_menu != NULL) {
		g_object_unref(s_tray_menu);
		s_tray_menu = NULL;
	}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	if (s_tray_icon != NULL) {
		g_object_unref(s_tray_icon);
		s_tray_icon = NULL;
	}
#pragma GCC diagnostic pop
	s_tray_show = NULL;
	s_tray_quit = NULL;
	s_tray_ud = NULL;
}

typedef struct hotkey_bind_data {
	platform_hotkey_arm_cb arm_cb;
	platform_hotkey_commit_cb commit_cb;
	void *userdata;
	int armed;
} hotkey_bind_data;

static int keyval_to_name(guint keyval, char *buf, size_t buf_sz)
{
	gunichar uc;
	int n;

	if (buf == NULL || buf_sz == 0)
		return -1;
	if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12) {
		snprintf(buf, buf_sz, "F%u", (unsigned)(keyval - GDK_KEY_F1 + 1));
		return 0;
	}
	switch (keyval) {
	case GDK_KEY_Escape:
		strncpy(buf, "Escape", buf_sz - 1);
		buf[buf_sz - 1] = '\0';
		return 0;
	case GDK_KEY_Delete:
		strncpy(buf, "Delete", buf_sz - 1);
		buf[buf_sz - 1] = '\0';
		return 0;
	case GDK_KEY_Insert:
		strncpy(buf, "Insert", buf_sz - 1);
		buf[buf_sz - 1] = '\0';
		return 0;
	case GDK_KEY_Home:
		strncpy(buf, "Home", buf_sz - 1);
		buf[buf_sz - 1] = '\0';
		return 0;
	case GDK_KEY_End:
		strncpy(buf, "End", buf_sz - 1);
		buf[buf_sz - 1] = '\0';
		return 0;
	case GDK_KEY_Page_Up:
		strncpy(buf, "PageUp", buf_sz - 1);
		buf[buf_sz - 1] = '\0';
		return 0;
	case GDK_KEY_Page_Down:
		strncpy(buf, "PageDown", buf_sz - 1);
		buf[buf_sz - 1] = '\0';
		return 0;
	case GDK_KEY_Up:
		strncpy(buf, "Up", buf_sz - 1);
		buf[buf_sz - 1] = '\0';
		return 0;
	case GDK_KEY_Down:
		strncpy(buf, "Down", buf_sz - 1);
		buf[buf_sz - 1] = '\0';
		return 0;
	case GDK_KEY_Left:
		strncpy(buf, "Left", buf_sz - 1);
		buf[buf_sz - 1] = '\0';
		return 0;
	case GDK_KEY_Right:
		strncpy(buf, "Right", buf_sz - 1);
		buf[buf_sz - 1] = '\0';
		return 0;
	default:
		break;
	}
	uc = gdk_keyval_to_unicode(keyval);
	if (uc == 0 || !g_unichar_isgraph(uc))
		return -1;
	uc = g_unichar_toupper(uc);
	n = g_unichar_to_utf8(uc, buf);
	if (n <= 0 || (size_t)n >= buf_sz)
		return -1;
	buf[n] = '\0';
	if (!g_utf8_validate(buf, -1, NULL))
		return -1;
	return 0;
}

static gboolean hotkey_focus_in(GtkWidget *widget, GdkEventFocus *event,
	gpointer data)
{
	hotkey_bind_data *bd;

	(void)widget;
	(void)event;
	bd = data;
	if (bd == NULL)
		return GDK_EVENT_PROPAGATE;
	bd->armed = 1;
	if (bd->arm_cb != NULL)
		bd->arm_cb(bd->userdata);
	return GDK_EVENT_PROPAGATE;
}

static gboolean hotkey_key_press(GtkWidget *widget, GdkEventKey *event,
	gpointer data)
{
	hotkey_bind_data *bd;
	char part[32];
	char out[128];
	size_t used;
	GtkWidget *top;

	(void)widget;
	bd = data;
	if (bd == NULL || !bd->armed)
		return GDK_EVENT_PROPAGATE;
	if (event == NULL)
		return GDK_EVENT_STOP;
	if (event->keyval == GDK_KEY_Control_L || event->keyval == GDK_KEY_Control_R ||
	    event->keyval == GDK_KEY_Shift_L || event->keyval == GDK_KEY_Shift_R ||
	    event->keyval == GDK_KEY_Alt_L || event->keyval == GDK_KEY_Alt_R ||
	    event->keyval == GDK_KEY_Meta_L || event->keyval == GDK_KEY_Meta_R ||
	    event->keyval == GDK_KEY_Super_L || event->keyval == GDK_KEY_Super_R)
		return GDK_EVENT_STOP;
	if (keyval_to_name(event->keyval, part, sizeof part) != 0)
		return GDK_EVENT_STOP;
	out[0] = '\0';
	if (event->state & GDK_SUPER_MASK)
		strncat(out, "Super+", sizeof out - strlen(out) - 1);
	if (event->state & GDK_CONTROL_MASK)
		strncat(out, "Ctrl+", sizeof out - strlen(out) - 1);
	if (event->state & GDK_MOD1_MASK)
		strncat(out, "Alt+", sizeof out - strlen(out) - 1);
	if (event->state & GDK_SHIFT_MASK)
		strncat(out, "Shift+", sizeof out - strlen(out) - 1);
	used = strlen(out);
	strncat(out, part, sizeof out - used - 1);
	bd->armed = 0;
	if (bd->commit_cb != NULL)
		bd->commit_cb(out, bd->userdata);
	top = gtk_widget_get_toplevel(widget);
	if (top != NULL && GTK_IS_WINDOW(top))
		gtk_window_set_focus(GTK_WINDOW(top), NULL);
	return GDK_EVENT_STOP;
}

int platform_bind_hotkey_entry(uiEntry *entry, platform_hotkey_arm_cb arm_cb,
	platform_hotkey_commit_cb commit_cb, void *userdata)
{
	GtkWidget *w;
	hotkey_bind_data *bd;

	if (entry == NULL)
		return -1;
	w = GTK_WIDGET(uiControlHandle(uiControl(entry)));
	if (w == NULL)
		return -1;
	bd = g_new0(hotkey_bind_data, 1);
	if (bd == NULL)
		return -1;
	bd->arm_cb = arm_cb;
	bd->commit_cb = commit_cb;
	bd->userdata = userdata;
	g_signal_connect(w, "focus-in-event", G_CALLBACK(hotkey_focus_in), bd);
	g_signal_connect(w, "key-press-event", G_CALLBACK(hotkey_key_press), bd);
	g_object_set_data_full(G_OBJECT(w), "clip-hotkey-bind-data", bd, g_free);
	return 0;
}
