#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include <errno.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <gdk/gdkkeysyms.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
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

int platform_user_state_path(char *buf, size_t buf_size)
{
	const char *st;
	const char *home;

	if (buf == NULL || buf_size == 0)
		return -1;
	st = getenv("XDG_STATE_HOME");
	if (st != NULL && st[0] != '\0') {
		if (snprintf(buf, buf_size, "%s/clipboard-manager/history.bin", st) >=
		    (int)buf_size)
			return -1;
		return 0;
	}
	home = getenv("HOME");
	if (home != NULL && home[0] != '\0') {
		if (snprintf(buf, buf_size,
		    "%s/.local/state/clipboard-manager/history.bin", home) >=
		    (int)buf_size)
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

int platform_ensure_state_parent(const char *utf8_path)
{
	return platform_ensure_config_parent(utf8_path);
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

static platform_global_hotkey_cb s_hotkey_cb;
static void *s_hotkey_ud;
#ifdef GDK_WINDOWING_X11
static Display *s_hotkey_dpy;
static Window s_hotkey_root;
static unsigned int s_hotkey_mods;
static unsigned int s_hotkey_lock_mask;
static int s_hotkey_keycode;
static int s_hotkey_active;
static int s_hotkey_grab_failed;
#endif

static void write_err(char *buf, size_t buf_size, const char *msg)
{
	if (buf == NULL || buf_size == 0)
		return;
	snprintf(buf, buf_size, "%s", msg);
}

static KeySym parse_hotkey_key(const char *name)
{
	if (name == NULL || name[0] == '\0')
		return NoSymbol;
	if (strlen(name) == 1) {
		char ch[2];
		ch[0] = name[0];
		ch[1] = '\0';
		return XStringToKeysym(ch);
	}
	if (strcasecmp(name, "Escape") == 0)
		return XK_Escape;
	if (strcasecmp(name, "Delete") == 0)
		return XK_Delete;
	if (strcasecmp(name, "Insert") == 0)
		return XK_Insert;
	if (strcasecmp(name, "Home") == 0)
		return XK_Home;
	if (strcasecmp(name, "End") == 0)
		return XK_End;
	if (strcasecmp(name, "PageUp") == 0)
		return XK_Page_Up;
	if (strcasecmp(name, "PageDown") == 0)
		return XK_Page_Down;
	if (strcasecmp(name, "Up") == 0)
		return XK_Up;
	if (strcasecmp(name, "Down") == 0)
		return XK_Down;
	if (strcasecmp(name, "Left") == 0)
		return XK_Left;
	if (strcasecmp(name, "Right") == 0)
		return XK_Right;
	if (name[0] == 'F' && name[1] != '\0') {
		int n = atoi(name + 1);
		if (n >= 1 && n <= 12)
			return XK_F1 + (n - 1);
	}
	return XStringToKeysym(name);
}

static int parse_hotkey_spec(const char *hotkey, unsigned int *mods_out,
	KeySym *keysym_out)
{
	char *copy;
	char *tok;
	char *saveptr;
	unsigned int mods;
	KeySym keysym;

	if (hotkey == NULL || hotkey[0] == '\0' || mods_out == NULL ||
	    keysym_out == NULL)
		return -1;
	copy = strdup(hotkey);
	if (copy == NULL)
		return -1;
	mods = 0U;
	keysym = NoSymbol;
	for (tok = strtok_r(copy, "+", &saveptr); tok != NULL;
	     tok = strtok_r(NULL, "+", &saveptr)) {
		if (strcasecmp(tok, "Ctrl") == 0 ||
		    strcasecmp(tok, "Control") == 0) {
			mods |= ControlMask;
			continue;
		}
		if (strcasecmp(tok, "Alt") == 0 ||
		    strcasecmp(tok, "Mod1") == 0) {
			mods |= Mod1Mask;
			continue;
		}
		if (strcasecmp(tok, "Shift") == 0) {
			mods |= ShiftMask;
			continue;
		}
		if (strcasecmp(tok, "Super") == 0 ||
		    strcasecmp(tok, "Meta") == 0) {
			mods |= Mod4Mask;
			continue;
		}
		keysym = parse_hotkey_key(tok);
	}
	free(copy);
	if (keysym == NoSymbol)
		return -1;
	*mods_out = mods;
	*keysym_out = keysym;
	return 0;
}

#ifdef GDK_WINDOWING_X11
static int x11_hotkey_error_handler(Display *dpy, XErrorEvent *ee)
{
	(void)dpy;
	if (ee != NULL && ee->error_code == BadAccess)
		s_hotkey_grab_failed = 1;
	return 0;
}

static unsigned int x11_numlock_mask(Display *dpy)
{
	XModifierKeymap *modmap;
	unsigned int mask;
	int mod;
	int key;

	modmap = XGetModifierMapping(dpy);
	if (modmap == NULL)
		return 0U;
	mask = 0U;
	for (mod = 0; mod < 8; mod++) {
		for (key = 0; key < modmap->max_keypermod; key++) {
			KeyCode code = modmap->modifiermap[mod * modmap->max_keypermod + key];
			KeySym sym;
			KeySym *map;
			int nsyms;
			if (code == 0)
				continue;
			map = XGetKeyboardMapping(dpy, code, 1, &nsyms);
			sym = (map != NULL && nsyms > 0) ? map[0] : NoSymbol;
			if (map != NULL)
				XFree(map);
			if (sym == XK_Num_Lock) {
				mask = (unsigned int)(1U << mod);
				break;
			}
		}
		if (mask != 0U)
			break;
	}
	XFreeModifiermap(modmap);
	return mask;
}

static void x11_grab_hotkey(Display *dpy, Window root, int keycode,
	unsigned int mods, unsigned int lock_mask)
{
	unsigned int variants[4];
	size_t i;

	variants[0] = mods;
	variants[1] = mods | LockMask;
	variants[2] = mods | lock_mask;
	variants[3] = mods | lock_mask | LockMask;
	for (i = 0; i < sizeof variants / sizeof variants[0]; i++) {
		XGrabKey(dpy, keycode, variants[i], root, True, GrabModeAsync,
			GrabModeAsync);
	}
}

static void x11_ungrab_hotkey(Display *dpy, Window root, int keycode,
	unsigned int mods, unsigned int lock_mask)
{
	unsigned int variants[4];
	size_t i;

	variants[0] = mods;
	variants[1] = mods | LockMask;
	variants[2] = mods | lock_mask;
	variants[3] = mods | lock_mask | LockMask;
	for (i = 0; i < sizeof variants / sizeof variants[0]; i++)
		XUngrabKey(dpy, keycode, variants[i], root);
}

static GdkFilterReturn global_hotkey_filter(GdkXEvent *xevent, GdkEvent *event,
	gpointer data)
{
	XEvent *xe;
	unsigned int clean_state;

	(void)event;
	(void)data;
	if (!s_hotkey_active || s_hotkey_cb == NULL)
		return GDK_FILTER_CONTINUE;
	xe = (XEvent *)xevent;
	if (xe == NULL || xe->type != KeyPress)
		return GDK_FILTER_CONTINUE;
	clean_state = (unsigned int)xe->xkey.state & ~(LockMask | s_hotkey_lock_mask);
	if (xe->xkey.keycode != (unsigned int)s_hotkey_keycode ||
	    clean_state != s_hotkey_mods)
		return GDK_FILTER_CONTINUE;
	s_hotkey_cb(s_hotkey_ud);
	return GDK_FILTER_REMOVE;
}
#endif

void platform_global_hotkey_clear(void)
{
#ifdef GDK_WINDOWING_X11
	if (s_hotkey_active && s_hotkey_dpy != NULL && s_hotkey_root != None &&
	    s_hotkey_keycode != 0) {
		x11_ungrab_hotkey(s_hotkey_dpy, s_hotkey_root, s_hotkey_keycode,
			s_hotkey_mods, s_hotkey_lock_mask);
		XSync(s_hotkey_dpy, False);
	}
	if (gdk_display_get_default() != NULL)
		gdk_window_remove_filter(NULL, global_hotkey_filter, NULL);
	s_hotkey_dpy = NULL;
	s_hotkey_root = None;
	s_hotkey_keycode = 0;
	s_hotkey_mods = 0U;
	s_hotkey_lock_mask = 0U;
	s_hotkey_active = 0;
#endif
	s_hotkey_cb = NULL;
	s_hotkey_ud = NULL;
}

int platform_global_hotkey_set(const char *hotkey,
	platform_global_hotkey_cb on_hotkey, void *userdata, char *errbuf,
	size_t errbuf_size)
{
#ifndef GDK_WINDOWING_X11
	(void)hotkey;
	(void)on_hotkey;
	(void)userdata;
	write_err(errbuf, errbuf_size, "Global hotkey requires X11.");
	return -1;
#else
	GdkDisplay *gd;
	unsigned int mods;
	KeySym ks;
	int keycode;
	XErrorHandler old_handler;

	if (hotkey == NULL || hotkey[0] == '\0' || on_hotkey == NULL) {
		write_err(errbuf, errbuf_size, "Empty hotkey.");
		return -1;
	}
	platform_global_hotkey_clear();
	gd = gdk_display_get_default();
	if (gd == NULL || !GDK_IS_X11_DISPLAY(gd)) {
		write_err(errbuf, errbuf_size,
			"Global hotkey is only supported on Linux X11.");
		return -1;
	}
	if (parse_hotkey_spec(hotkey, &mods, &ks) != 0) {
		write_err(errbuf, errbuf_size, "Invalid hotkey format.");
		return -1;
	}
	s_hotkey_dpy = gdk_x11_display_get_xdisplay(gd);
	if (s_hotkey_dpy == NULL) {
		write_err(errbuf, errbuf_size, "X11 display is unavailable.");
		return -1;
	}
	keycode = XKeysymToKeycode(s_hotkey_dpy, ks);
	if (keycode == 0) {
		write_err(errbuf, errbuf_size, "Unsupported hotkey key.");
		return -1;
	}
	s_hotkey_root = DefaultRootWindow(s_hotkey_dpy);
	s_hotkey_mods = mods;
	s_hotkey_keycode = keycode;
	s_hotkey_lock_mask = x11_numlock_mask(s_hotkey_dpy);
	s_hotkey_grab_failed = 0;
	old_handler = XSetErrorHandler(x11_hotkey_error_handler);
	x11_grab_hotkey(s_hotkey_dpy, s_hotkey_root, s_hotkey_keycode, s_hotkey_mods,
		s_hotkey_lock_mask);
	XSync(s_hotkey_dpy, False);
	XSetErrorHandler(old_handler);
	if (s_hotkey_grab_failed) {
		write_err(errbuf, errbuf_size,
			"Hotkey is already used by another application.");
		platform_global_hotkey_clear();
		return -1;
	}
	gdk_window_add_filter(NULL, global_hotkey_filter, NULL);
	s_hotkey_cb = on_hotkey;
	s_hotkey_ud = userdata;
	s_hotkey_active = 1;
	write_err(errbuf, errbuf_size, "Global hotkey enabled.");
	return 0;
#endif
}

static GtkWidget *s_picker_win;
static GtkWidget *s_picker_view;
static GtkListStore *s_picker_store;
static platform_picker_choose_cb s_picker_choose_cb;
static platform_picker_cancel_cb s_picker_cancel_cb;
static void *s_picker_ud;

static void quick_picker_finish_with_token(uint64_t token)
{
	if (s_picker_choose_cb != NULL)
		s_picker_choose_cb(token, s_picker_ud);
}

static gboolean quick_picker_on_key(GtkWidget *widget, GdkEventKey *event,
	gpointer data)
{
	GtkTreeSelection *sel;
	GtkTreeModel *model;
	GtkTreeIter it;
	guint64 token;

	(void)data;
	if (event == NULL)
		return GDK_EVENT_PROPAGATE;
	if (event->keyval == GDK_KEY_Escape) {
		if (s_picker_cancel_cb != NULL)
			s_picker_cancel_cb(s_picker_ud);
		return GDK_EVENT_STOP;
	}
	if (event->keyval != GDK_KEY_Return && event->keyval != GDK_KEY_KP_Enter)
		return GDK_EVENT_PROPAGATE;
	sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
	if (!gtk_tree_selection_get_selected(sel, &model, &it))
		return GDK_EVENT_STOP;
	gtk_tree_model_get(model, &it, 1, &token, -1);
	quick_picker_finish_with_token((uint64_t)token);
	return GDK_EVENT_STOP;
}

static void quick_picker_on_row_activated(GtkTreeView *tree_view,
	GtkTreePath *path, GtkTreeViewColumn *column, gpointer data)
{
	GtkTreeModel *model;
	GtkTreeIter it;
	guint64 token;

	(void)tree_view;
	(void)column;
	(void)data;
	model = GTK_TREE_MODEL(s_picker_store);
	if (path == NULL || !gtk_tree_model_get_iter(model, &it, path))
		return;
	gtk_tree_model_get(model, &it, 1, &token, -1);
	quick_picker_finish_with_token((uint64_t)token);
}

static gboolean quick_picker_on_focus_out(GtkWidget *widget, GdkEvent *event,
	gpointer data)
{
	(void)widget;
	(void)event;
	(void)data;
	if (s_picker_cancel_cb != NULL)
		s_picker_cancel_cb(s_picker_ud);
	return GDK_EVENT_PROPAGATE;
}

void platform_quick_picker_hide(void)
{
	if (s_picker_win != NULL) {
		gtk_widget_destroy(s_picker_win);
		s_picker_win = NULL;
	}
	s_picker_view = NULL;
	s_picker_store = NULL;
	s_picker_choose_cb = NULL;
	s_picker_cancel_cb = NULL;
	s_picker_ud = NULL;
}

int platform_quick_picker_show(const platform_picker_item *items, size_t count,
	platform_picker_choose_cb choose_cb, platform_picker_cancel_cb cancel_cb,
	void *userdata)
{
	GtkWidget *scroll;
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *col;
	GdkDisplay *display;
	GdkSeat *seat;
	GdkDevice *pointer;
	gint cx;
	gint cy;
	GtkTreeSelection *sel;
	GtkTreeIter it;
	size_t i;

	if (items == NULL || count == 0 || choose_cb == NULL)
		return -1;
	platform_quick_picker_hide();
	s_picker_choose_cb = choose_cb;
	s_picker_cancel_cb = cancel_cb;
	s_picker_ud = userdata;
	s_picker_win = gtk_window_new(GTK_WINDOW_POPUP);
	gtk_window_set_decorated(GTK_WINDOW(s_picker_win), FALSE);
	gtk_window_set_keep_above(GTK_WINDOW(s_picker_win), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(s_picker_win), 420, 280);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	s_picker_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_UINT64);
	for (i = 0; i < count; i++) {
		gtk_list_store_append(s_picker_store, &it);
		gtk_list_store_set(s_picker_store, &it,
			0, items[i].label != NULL ? items[i].label : "",
			1, (guint64)items[i].token,
			-1);
	}
	s_picker_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s_picker_store));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(s_picker_view), FALSE);
	gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(s_picker_view), TRUE);
	renderer = gtk_cell_renderer_text_new();
	col = gtk_tree_view_column_new_with_attributes("Item", renderer, "text", 0,
		NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(s_picker_view), col);
	gtk_container_add(GTK_CONTAINER(scroll), s_picker_view);
	gtk_container_add(GTK_CONTAINER(s_picker_win), scroll);
	g_signal_connect(s_picker_view, "row-activated",
		G_CALLBACK(quick_picker_on_row_activated), NULL);
	g_signal_connect(s_picker_view, "key-press-event",
		G_CALLBACK(quick_picker_on_key), NULL);
	g_signal_connect(s_picker_win, "focus-out-event",
		G_CALLBACK(quick_picker_on_focus_out), NULL);
	gtk_widget_show_all(s_picker_win);
	display = gdk_display_get_default();
	cx = 120;
	cy = 120;
	if (display != NULL) {
		seat = gdk_display_get_default_seat(display);
		if (seat != NULL) {
			pointer = gdk_seat_get_pointer(seat);
			if (pointer != NULL)
				gdk_device_get_position(pointer, NULL, &cx, &cy);
		}
	}
	gtk_window_move(GTK_WINDOW(s_picker_win), cx + 8, cy + 8);
	sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(s_picker_view));
	gtk_tree_selection_set_mode(sel, GTK_SELECTION_BROWSE);
	if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(s_picker_store), &it))
		gtk_tree_selection_select_iter(sel, &it);
	gtk_widget_grab_focus(s_picker_view);
	return 0;
}

int platform_simulate_paste(void)
{
#ifdef GDK_WINDOWING_X11
	GdkDisplay *gd;
	Display *dpy;
	KeyCode ctrl;
	KeyCode v;
	int event_base;
	int error_base;
	int major;
	int minor;

	gd = gdk_display_get_default();
	if (gd == NULL || !GDK_IS_X11_DISPLAY(gd))
		return -1;
	dpy = gdk_x11_display_get_xdisplay(gd);
	if (dpy == NULL)
		return -1;
	if (!XTestQueryExtension(dpy, &event_base, &error_base, &major, &minor))
		return -1;
	ctrl = XKeysymToKeycode(dpy, XK_Control_L);
	v = XKeysymToKeycode(dpy, XK_v);
	if (ctrl == 0 || v == 0)
		return -1;
	XTestFakeKeyEvent(dpy, ctrl, True, CurrentTime);
	XTestFakeKeyEvent(dpy, v, True, CurrentTime);
	XTestFakeKeyEvent(dpy, v, False, CurrentTime);
	XTestFakeKeyEvent(dpy, ctrl, False, CurrentTime);
	XFlush(dpy);
	return 0;
#else
	return -1;
#endif
}

static int s_ipc_fd = -1;
static guint s_ipc_source_id;
static char s_ipc_path[108];
static platform_ipc_picker_cb s_ipc_picker_cb;
static void *s_ipc_ud;

static void ipc_write_err(char *buf, size_t buf_size, const char *msg)
{
	write_err(buf, buf_size, msg);
}

static int ipc_build_path(char *buf, size_t buf_size)
{
	const char *rt;
	uid_t uid;

	if (buf == NULL || buf_size == 0)
		return -1;
	rt = getenv("XDG_RUNTIME_DIR");
	if (rt != NULL && rt[0] != '\0') {
		if (snprintf(buf, buf_size, "%s/clipboard-manager.sock", rt) >=
		    (int)buf_size)
			return -1;
		return 0;
	}
	uid = getuid();
	if (snprintf(buf, buf_size, "/tmp/clipboard-manager-%lu.sock",
		(unsigned long)uid) >= (int)buf_size)
		return -1;
	return 0;
}

static gboolean ipc_on_io(gint fd, GIOCondition cond, gpointer data)
{
	int cfd;
	char buf[64];
	ssize_t n;
	struct sockaddr_un addr;
	socklen_t alen;

	(void)data;
	if ((cond & (G_IO_HUP | G_IO_ERR)) != 0)
		return G_SOURCE_CONTINUE;
	if ((cond & G_IO_IN) == 0)
		return G_SOURCE_CONTINUE;
	for (;;) {
		alen = sizeof addr;
		cfd = accept(fd, (struct sockaddr *)&addr, &alen);
		if (cfd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			break;
		}
		n = read(cfd, buf, sizeof buf - 1);
		if (n > 0) {
			buf[n] = '\0';
			if (strncmp(buf, "picker", 6) == 0 && s_ipc_picker_cb != NULL)
				s_ipc_picker_cb(s_ipc_ud);
		}
		close(cfd);
	}
	return G_SOURCE_CONTINUE;
}

void platform_ipc_server_stop(void)
{
	if (s_ipc_source_id != 0) {
		g_source_remove(s_ipc_source_id);
		s_ipc_source_id = 0;
	}
	if (s_ipc_fd >= 0) {
		close(s_ipc_fd);
		s_ipc_fd = -1;
	}
	if (s_ipc_path[0] != '\0') {
		(void)unlink(s_ipc_path);
		s_ipc_path[0] = '\0';
	}
	s_ipc_picker_cb = NULL;
	s_ipc_ud = NULL;
}

int platform_ipc_server_start(platform_ipc_picker_cb on_picker, void *userdata,
	char *errbuf, size_t errbuf_size)
{
	struct sockaddr_un addr;
	int fd;

	if (on_picker == NULL) {
		ipc_write_err(errbuf, errbuf_size, "IPC picker callback missing.");
		return -1;
	}
	platform_ipc_server_stop();
	if (ipc_build_path(s_ipc_path, sizeof s_ipc_path) != 0) {
		ipc_write_err(errbuf, errbuf_size, "Failed to determine IPC path.");
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		ipc_write_err(errbuf, errbuf_size, "Failed to create IPC socket.");
		s_ipc_path[0] = '\0';
		return -1;
	}
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", s_ipc_path);
	(void)unlink(s_ipc_path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		close(fd);
		s_ipc_path[0] = '\0';
		ipc_write_err(errbuf, errbuf_size,
			"Failed to bind IPC socket (already running?).");
		return -1;
	}
	if (listen(fd, 8) != 0) {
		close(fd);
		(void)unlink(s_ipc_path);
		s_ipc_path[0] = '\0';
		ipc_write_err(errbuf, errbuf_size, "Failed to listen on IPC socket.");
		return -1;
	}
	s_ipc_fd = fd;
	s_ipc_picker_cb = on_picker;
	s_ipc_ud = userdata;
	s_ipc_source_id = g_unix_fd_add(s_ipc_fd, G_IO_IN | G_IO_ERR | G_IO_HUP,
		ipc_on_io, NULL);
	ipc_write_err(errbuf, errbuf_size, "IPC server enabled.");
	return 0;
}

int platform_ipc_request_picker(char *errbuf, size_t errbuf_size)
{
	char path[108];
	struct sockaddr_un addr;
	int fd;
	int r;
	ssize_t n;
	const char *msg = "picker\n";

	if (ipc_build_path(path, sizeof path) != 0) {
		ipc_write_err(errbuf, errbuf_size, "Failed to determine IPC path.");
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		ipc_write_err(errbuf, errbuf_size, "Failed to create IPC client socket.");
		return -1;
	}
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
	r = connect(fd, (struct sockaddr *)&addr, sizeof addr);
	if (r != 0) {
		close(fd);
		ipc_write_err(errbuf, errbuf_size, "No running instance (IPC connect failed).");
		return -1;
	}
	n = write(fd, msg, strlen(msg));
	close(fd);
	if (n <= 0) {
		ipc_write_err(errbuf, errbuf_size, "Failed to send IPC request.");
		return -1;
	}
	ipc_write_err(errbuf, errbuf_size, "Picker requested.");
	return 0;
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
