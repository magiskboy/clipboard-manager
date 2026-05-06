#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ui.h>

#include "clipboard_manager.h"
#include "platform.h"

typedef struct app_state {
	clip_store *store;
	uiTableModel *model;
	uiTable *table;
	uiWindow *win;
	uiWindow *detail_win;
	uiMultilineEntry *detail_text;
	char config_path[512];
	clip_app_config cfg;
	uiWindow *settings_win;
	uiWindow *about_win;
	uiSpinbox *max_spin;
	uiEntry *hotkey_entry;
	uiLabel *hotkey_hint_label;
	uiButton *settings_save_btn;
	uiButton *settings_cancel_btn;
	clip_app_config settings_draft;
} app_state;

static app_state g_app;

static void app_save_config(app_state *a);
static void app_shutdown_services(void);
static void open_settings_window(app_state *a);
static void open_about_window(app_state *a);
static void on_menu_clear_all(uiMenuItem *item, uiWindow *win, void *data);

static int mh_numcolumns(uiTableModelHandler *mh, uiTableModel *m)
{
	(void)mh;
	(void)m;
	return 1;
}

static uiTableValueType mh_columntype(uiTableModelHandler *mh, uiTableModel *m,
	int col)
{
	(void)mh;
	(void)m;
	(void)col;
	return uiTableValueTypeString;
}

static int mh_numrows(uiTableModelHandler *mh, uiTableModel *m)
{
	(void)mh;
	(void)m;
	return (int)clip_store_count(g_app.store);
}

static uiTableValue *mh_cellvalue(uiTableModelHandler *mh, uiTableModel *m,
	int row, int col)
{
	const clip_item *it;
	char buf[192];

	(void)mh;
	(void)m;
	it = clip_store_get(g_app.store, (size_t)row);
	if (it == NULL)
		return uiNewTableValueString("");
	if (col != 0)
		return uiNewTableValueString("");
	clip_item_format_preview(it, buf, sizeof(buf));
	return uiNewTableValueString(buf);
}

static void mh_setcellvalue(uiTableModelHandler *mh, uiTableModel *m, int row,
	int col, const uiTableValue *val)
{
	(void)mh;
	(void)m;
	(void)row;
	(void)col;
	(void)val;
}

static uiTableModelHandler g_mh = {
	mh_numcolumns,
	mh_columntype,
	mh_numrows,
	mh_cellvalue,
	mh_setcellvalue,
};

static void app_save_config(app_state *a)
{
	if (a == NULL || a->config_path[0] == '\0')
		return;
	if (platform_ensure_config_parent(a->config_path) != 0)
		return;
	if (clip_config_save(a->config_path, &a->cfg) != 0)
		fprintf(stderr, "Failed to save config.\n");
}

static void app_shutdown_services(void)
{
	platform_clipboard_shutdown();
	platform_tray_shutdown();
}

static int on_should_quit(void *data)
{
	(void)data;
	app_shutdown_services();
	return 1;
}

static void tray_show_main(void *ud)
{
	app_state *a = ud;

	if (a != NULL && a->win != NULL)
		uiControlShow(uiControl(a->win));
}

static void tray_quit_app(void *ud)
{
	(void)ud;
	app_shutdown_services();
	uiQuit();
}

static void on_hotkey_capture_arm(void *userdata)
{
	app_state *a = userdata;

	if (a == NULL || a->hotkey_entry == NULL || a->hotkey_hint_label == NULL)
		return;
	uiEntrySetText(a->hotkey_entry, "Press hotkey...");
	uiLabelSetText(a->hotkey_hint_label, "Waiting for key combination...");
}

static void on_hotkey_capture_commit(const char *hotkey, void *userdata)
{
	app_state *a = userdata;

	if (a == NULL || hotkey == NULL)
		return;
	uiEntrySetText(a->hotkey_entry, hotkey);
	strncpy(a->settings_draft.hotkey, hotkey, CLIP_HOTKEY_MAX);
	a->settings_draft.hotkey[CLIP_HOTKEY_MAX] = '\0';
	uiLabelSetText(a->hotkey_hint_label,
		"Captured. Press Save to apply.");
}

static void on_max_spin_changed(uiSpinbox *s, void *data)
{
	app_state *a = data;
	int v;

	if (a == NULL)
		return;
	v = uiSpinboxValue(s);
	if (v < (int)CLIP_STORE_MIN_ITEMS)
		v = (int)CLIP_STORE_MIN_ITEMS;
	if (v > (int)CLIP_STORE_CAP_MAX_ITEMS)
		v = (int)CLIP_STORE_CAP_MAX_ITEMS;
	a->settings_draft.max_items = (size_t)v;
	uiLabelSetText(a->hotkey_hint_label,
		"Changed. Press Save to apply.");
}

static void settings_reset_ui_from_draft(app_state *a)
{
	if (a == NULL)
		return;
	uiSpinboxSetValue(a->max_spin, (int)a->settings_draft.max_items);
	uiEntrySetText(a->hotkey_entry, a->settings_draft.hotkey);
}

static void on_settings_save(uiButton *b, void *data)
{
	app_state *a = data;
	size_t oldc;
	size_t newc;
	size_t i;

	(void)b;
	if (a == NULL || a->store == NULL || a->model == NULL)
		return;
	oldc = clip_store_count(a->store);
	clip_store_set_max_items(a->store, a->settings_draft.max_items);
	newc = clip_store_count(a->store);
	for (i = oldc; i > newc; i--)
		uiTableModelRowDeleted(a->model, (int)(i - 1));
	a->cfg = a->settings_draft;
	app_save_config(a);
	uiLabelSetText(a->hotkey_hint_label,
		"Saved. Global registration is planned for Phase 3.");
	uiControlHide(uiControl(a->settings_win));
}

static void on_settings_cancel(uiButton *b, void *data)
{
	app_state *a = data;

	(void)b;
	if (a == NULL)
		return;
	a->settings_draft = a->cfg;
	settings_reset_ui_from_draft(a);
	uiLabelSetText(a->hotkey_hint_label,
		"Cancelled.");
	uiControlHide(uiControl(a->settings_win));
}

static int on_settings_closing(uiWindow *w, void *data)
{
	app_state *a = data;

	(void)w;
	on_settings_cancel(NULL, a);
	return 0;
}

static int on_about_closing(uiWindow *w, void *data)
{
	app_state *a = data;

	(void)w;
	if (a != NULL)
		uiControlHide(uiControl(a->about_win));
	return 0;
}

static void ensure_settings_window(app_state *a)
{
	uiBox *root;
	uiBox *actions;
	uiLabel *lbl;

	if (a->settings_win != NULL)
		return;
	a->settings_win = uiNewWindow("Settings", 340, 190, 0);
	uiWindowSetMargined(a->settings_win, 1);
	root = uiNewVerticalBox();
	lbl = uiNewLabel("Maximum history items (applied immediately):");
	uiBoxAppend(root, uiControl(lbl), 0);
	a->max_spin = uiNewSpinbox((int)CLIP_STORE_MIN_ITEMS,
	    (int)CLIP_STORE_CAP_MAX_ITEMS);
	uiSpinboxSetValue(a->max_spin,
	    (int)clip_store_get_max_items(a->store));
	uiSpinboxOnChanged(a->max_spin, on_max_spin_changed, a);
	uiBoxAppend(root, uiControl(a->max_spin), 0);
	lbl = uiNewLabel("Global shortcut:");
	uiBoxAppend(root, uiControl(lbl), 0);
	a->hotkey_entry = uiNewEntry();
	uiBoxAppend(root, uiControl(a->hotkey_entry), 0);
	platform_bind_hotkey_entry(a->hotkey_entry, on_hotkey_capture_arm,
		on_hotkey_capture_commit, a);
	a->hotkey_hint_label = uiNewLabel(
	    "Click input and press key combination.");
	uiBoxAppend(root, uiControl(a->hotkey_hint_label), 0);
	actions = uiNewHorizontalBox();
	uiBoxSetPadded(actions, 1);
	a->settings_save_btn = uiNewButton("Save");
	uiButtonOnClicked(a->settings_save_btn, on_settings_save, a);
	uiBoxAppend(actions, uiControl(a->settings_save_btn), 0);
	a->settings_cancel_btn = uiNewButton("Cancel");
	uiButtonOnClicked(a->settings_cancel_btn, on_settings_cancel, a);
	uiBoxAppend(actions, uiControl(a->settings_cancel_btn), 0);
	uiBoxAppend(root, uiControl(actions), 0);
	uiWindowSetChild(a->settings_win, uiControl(root));
	uiWindowOnClosing(a->settings_win, on_settings_closing, a);
}

static void ensure_about_window(app_state *a)
{
	uiGrid *grid;
	uiLabel *label;
	char text[512];

	if (a->about_win != NULL)
		return;
	a->about_win = uiNewWindow("About", 360, 200, 0);
	uiWindowSetMargined(a->about_win, 1);
	grid = uiNewGrid();
	snprintf(text, sizeof text,
	    "%s\nVersion %s\n\n"
	    "Cross-platform clipboard history.\n"
	    "Developed with libui-ng.",
	    CLIP_APP_NAME, CLIP_APP_VERSION);
	if (CLIP_REPO_URL[0] != '\0') {
		strncat(text, "\n\n", sizeof text - strlen(text) - 1);
		strncat(text, CLIP_REPO_URL, sizeof text - strlen(text) - 1);
	}
	label = uiNewLabel(text);
	uiGridAppend(grid, uiControl(label), 0, 0, 1, 1,
		1, uiAlignCenter, 1, uiAlignCenter);
	uiWindowSetChild(a->about_win, uiControl(grid));
	uiWindowOnClosing(a->about_win, on_about_closing, a);
}

static void open_settings_window(app_state *a)
{
	if (a == NULL)
		return;
	ensure_settings_window(a);
	a->settings_draft = a->cfg;
	settings_reset_ui_from_draft(a);
	uiLabelSetText(a->hotkey_hint_label,
		"Click input, press key combination, then Save.");
	uiControlShow(uiControl(a->settings_win));
}

static void open_about_window(app_state *a)
{
	if (a == NULL)
		return;
	ensure_about_window(a);
	uiControlShow(uiControl(a->about_win));
}

static void on_menu_preferences(uiMenuItem *item, uiWindow *w, void *data)
{
	(void)item;
	(void)w;
	open_settings_window((app_state *)data);
}

static void on_menu_about(uiMenuItem *item, uiWindow *w, void *data)
{
	(void)item;
	(void)w;
	open_about_window((app_state *)data);
}

static void on_menu_clear_all(uiMenuItem *item, uiWindow *win, void *data)
{
	app_state *a = data;
	size_t n;
	size_t i;

	(void)item;
	(void)win;
	if (a == NULL || a->store == NULL || a->model == NULL)
		return;
	n = clip_store_count(a->store);
	clip_store_clear(a->store);
	for (i = 0; i < n; i++)
		uiTableModelRowDeleted(a->model, 0);
}

static void on_clipboard_changed(const char *utf8, const char *source_app,
	void *ud)
{
	app_state *a = ud;
	size_t before;
	size_t after;

	if (a == NULL || a->store == NULL || a->model == NULL)
		return;
	before = clip_store_count(a->store);
	clip_store_add_text(a->store, utf8, source_app);
	after = clip_store_count(a->store);
	if (after > before)
		uiTableModelRowInserted(a->model, 0);
}

static int on_clipboard_timer(void *data)
{
	(void)data;
	platform_clipboard_poll();
	return 1;
}

static int on_detail_closing(uiWindow *w, void *data)
{
	app_state *a = data;

	(void)w;
	if (a != NULL) {
		a->detail_win = NULL;
		a->detail_text = NULL;
	}
	return 1;
}

static void ensure_detail_window(app_state *a)
{
	uiBox *root;

	if (a->detail_win != NULL)
		return;
	a->detail_win = uiNewWindow("Clipboard entry", 520, 360, 0);
	uiWindowSetMargined(a->detail_win, 1);
	a->detail_text = uiNewMultilineEntry();
	uiMultilineEntrySetReadOnly(a->detail_text, 1);
	root = uiNewVerticalBox();
	uiBoxSetPadded(root, 0);
	uiBoxAppend(root, uiControl(a->detail_text), 1);
	uiWindowSetChild(a->detail_win, uiControl(root));
	uiWindowOnClosing(a->detail_win, on_detail_closing, a);
}

static void open_detail_for_row(app_state *a, int row)
{
	const clip_item *it;

	if (a == NULL || row < 0)
		return;
	it = clip_store_get(a->store, (size_t)row);
	if (it == NULL)
		return;
	ensure_detail_window(a);
	uiMultilineEntrySetText(a->detail_text, it->text);
	uiControlShow(uiControl(a->detail_win));
}

static void on_row_double_clicked(uiTable *t, int row, void *data)
{
	app_state *a = data;

	(void)t;
	open_detail_for_row(a, row);
}

static int on_main_closing(uiWindow *w, void *data)
{
	app_state *a = data;

	(void)w;
	if (a != NULL && a->win != NULL)
		uiControlHide(uiControl(a->win));
	return 0;
}

int main(int argc, char *argv[])
{
	uiInitOptions o;
	const char *err;
	uiWindow *w;
	uiTableParams tp;
	uiTable *table;
	uiBox *main_box;
	const unsigned timer_ms = 400;
	uiMenu *mfile;
	uiMenu *medit;
	uiMenu *mhelp;
	uiMenuItem *prefs_item;
	uiMenuItem *clear_item;
	uiMenuItem *about_item;

	(void)argc;
	(void)argv;
	memset(&g_app, 0, sizeof g_app);
	memset(&o, 0, sizeof o);
	err = uiInit(&o);
	if (err != NULL) {
		fprintf(stderr, "%s\n", err);
		uiFreeInitError(err);
		return 1;
	}

	if (platform_user_config_path(g_app.config_path,
	    sizeof g_app.config_path) != 0)
		g_app.config_path[0] = '\0';
	if (g_app.config_path[0] != '\0')
		clip_config_load(g_app.config_path, &g_app.cfg);
	else
		clip_config_default(&g_app.cfg);

	g_app.store = clip_store_new(g_app.cfg.max_items);
	if (g_app.store == NULL) {
		fprintf(stderr, "Out of memory.\n");
		uiUninit();
		return 1;
	}

	g_app.model = uiNewTableModel(&g_mh);
	if (g_app.model == NULL) {
		clip_store_free(g_app.store);
		uiUninit();
		return 1;
	}

	mfile = uiNewMenu("File");
	uiMenuAppendQuitItem(mfile);
	medit = uiNewMenu("Edit");
	prefs_item = uiMenuAppendPreferencesItem(medit);
	uiMenuItemOnClicked(prefs_item, on_menu_preferences, &g_app);
	uiMenuAppendSeparator(medit);
	clear_item = uiMenuAppendItem(medit, "Clear all");
	uiMenuItemOnClicked(clear_item, on_menu_clear_all, &g_app);
	mhelp = uiNewMenu("Help");
	about_item = uiMenuAppendAboutItem(mhelp);
	uiMenuItemOnClicked(about_item, on_menu_about, &g_app);

	w = uiNewWindow(CLIP_APP_NAME, 320, 500, 1);
	g_app.win = w;
	uiWindowOnClosing(w, on_main_closing, &g_app);
	uiOnShouldQuit(on_should_quit, &g_app);
	uiWindowSetResizeable(w, 0);
	uiWindowSetMargined(w, 1);

	memset(&tp, 0, sizeof(tp));
	tp.Model = g_app.model;
	tp.RowBackgroundColorModelColumn = -1;
	table = uiNewTable(&tp);
	g_app.table = table;
	uiTableSetSelectionMode(table, uiTableSelectionModeZeroOrOne);
	uiTableAppendTextColumn(table, "", 0,
		uiTableModelColumnNeverEditable, NULL);
	uiTableHeaderSetVisible(table, 0);
	platform_polish_clipboard_list(table);
	/*
	 * On GTK, a control added directly as the window child is packed into a
	 * GtkBox without expand, so the table stays minimum width. A stretchy box
	 * child gets hexpand/vexpand and fills the content area.
	 */
	main_box = uiNewVerticalBox();
	uiBoxSetPadded(main_box, 0);
	uiBoxAppend(main_box, uiControl(table), 1);
	uiWindowSetChild(w, uiControl(main_box));
	uiTableOnRowDoubleClicked(table, on_row_double_clicked, &g_app);

	platform_clipboard_init(on_clipboard_changed, &g_app);
	platform_tray_init(w, tray_show_main, tray_quit_app, &g_app);
	uiTimer(timer_ms, on_clipboard_timer, NULL);

	uiControlShow(uiControl(w));
	uiMain();

	clip_store_free(g_app.store);
	g_app.store = NULL;
	uiFreeTableModel(g_app.model);
	g_app.model = NULL;
	uiUninit();
	return 0;
}
