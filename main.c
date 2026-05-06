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
	char state_path[512];
	int history_dirty;
	clip_app_config cfg;
	uiWindow *settings_win;
	uiWindow *about_win;
	uiSpinbox *max_spin;
	uiSpinbox *clip_bytes_spin;
	uiCheckbox *persist_checkbox;
	uiEntry *hotkey_entry;
	uiLabel *hotkey_hint_label;
	uiLabel *persist_hint_label;
	uiButton *settings_save_btn;
	uiButton *settings_cancel_btn;
	clip_app_config settings_draft;
	platform_picker_item quick_items[32];
	char quick_labels[32][192];
	size_t quick_count;
} app_state;

static app_state g_app;

static void app_save_config(app_state *a);
static void app_mark_history_dirty(app_state *a);
static void app_flush_history(app_state *a);
static void app_shutdown_services(void);
static void app_on_global_hotkey(void *userdata);
static void app_on_picker_choose(uint64_t token, void *userdata);
static void app_on_picker_cancel(void *userdata);
static int app_rebind_global_hotkey(app_state *a, char *reason,
	size_t reason_size);
static void app_open_quick_picker(app_state *a);
static void app_on_ipc_picker(void *userdata);
static void app_on_ipc_picker_queued(void *userdata);
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

static void app_mark_history_dirty(app_state *a)
{
	if (a == NULL || !a->cfg.persist_history || a->state_path[0] == '\0')
		return;
	a->history_dirty = 1;
}

static void app_flush_history(app_state *a)
{
	if (a == NULL || a->store == NULL || !a->cfg.persist_history ||
	    a->state_path[0] == '\0')
		return;
	if (!a->history_dirty)
		return;
	if (platform_ensure_state_parent(a->state_path) != 0) {
		fprintf(stderr, "Failed to create state directory.\n");
		return;
	}
	if (clip_history_save(a->store, a->state_path) != 0)
		fprintf(stderr, "Failed to save clipboard history.\n");
	else
		a->history_dirty = 0;
}

static void app_shutdown_services(void)
{
	platform_quick_picker_hide();
	platform_global_hotkey_clear();
	platform_ipc_server_stop();
	platform_clipboard_shutdown();
	platform_tray_shutdown();
}

static int app_find_row_by_item_id(app_state *a, uint64_t id)
{
	size_t n;
	size_t i;
	const clip_item *it;

	if (a == NULL || a->store == NULL)
		return -1;
	n = clip_store_count(a->store);
	for (i = 0; i < n; i++) {
		it = clip_store_get(a->store, i);
		if (it != NULL && it->id == id)
			return (int)i;
	}
	return -1;
}

static void app_copy_and_paste_item(app_state *a, uint64_t id)
{
	int row;
	const clip_item *it;

	if (a == NULL || a->store == NULL)
		return;
	row = app_find_row_by_item_id(a, id);
	if (row < 0)
		return;
	it = clip_store_get(a->store, (size_t)row);
	if (it == NULL || it->text == NULL || it->text[0] == '\0')
		return;
	clip_store_mark_self_copy(a->store, it->text);
	if (platform_clipboard_set_text(it->text) != 0) {
		fprintf(stderr, "Failed to write selected text to clipboard.\n");
		return;
	}
	if (platform_simulate_paste() != 0)
		fprintf(stderr,
			"Auto-paste unavailable; text is on clipboard for manual paste.\n");
}

static void app_on_picker_choose(uint64_t token, void *userdata)
{
	app_state *a = userdata;

	/*
	 * On Wayland, clipboard ownership is tied to focused surfaces. Keep the
	 * picker focused while committing clipboard data, then close it.
	 */
	app_copy_and_paste_item(a, token);
	platform_quick_picker_hide();
}

static void app_on_picker_cancel(void *userdata)
{
	(void)userdata;
	platform_quick_picker_hide();
}

static void app_open_quick_picker(app_state *a)
{
	size_t n;
	size_t i;
	const clip_item *it;
	size_t cap;

	if (a == NULL || a->store == NULL)
		return;
	n = clip_store_count(a->store);
	if (n == 0)
		return;
	cap = sizeof a->quick_items / sizeof a->quick_items[0];
	if (n > cap)
		n = cap;
	for (i = 0; i < n; i++) {
		it = clip_store_get(a->store, i);
		if (it == NULL)
			continue;
		clip_item_format_preview(it, a->quick_labels[i],
			sizeof a->quick_labels[i]);
		a->quick_items[i].label = a->quick_labels[i];
		a->quick_items[i].token = it->id;
	}
	a->quick_count = n;
	if (platform_quick_picker_show(a->quick_items, a->quick_count,
		app_on_picker_choose, app_on_picker_cancel, a) != 0)
		fprintf(stderr, "Failed to show quick picker.\n");
}

static void app_on_global_hotkey(void *userdata)
{
	app_open_quick_picker((app_state *)userdata);
}

static void app_on_ipc_picker(void *userdata)
{
	uiQueueMain(app_on_ipc_picker_queued, userdata);
}

static void app_on_ipc_picker_queued(void *userdata)
{
	app_state *a = userdata;

	app_open_quick_picker(a);
}

static int app_rebind_global_hotkey(app_state *a, char *reason,
	size_t reason_size)
{
	if (a == NULL)
		return -1;
	platform_global_hotkey_clear();
	if (reason != NULL && reason_size > 0)
		reason[0] = '\0';
	if (a->cfg.hotkey[0] == '\0')
		return 0;
	return platform_global_hotkey_set(a->cfg.hotkey, app_on_global_hotkey, a,
		reason, reason_size);
}

static int on_should_quit(void *data)
{
	app_state *a = data;

	if (a != NULL)
		app_flush_history(a);
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

static void on_clip_bytes_spin_changed(uiSpinbox *s, void *data)
{
	app_state *a = data;
	int v;

	if (a == NULL)
		return;
	v = uiSpinboxValue(s);
	if (v < (int)CLIP_TEXT_BYTES_MIN)
		v = (int)CLIP_TEXT_BYTES_MIN;
	if (v > (int)CLIP_TEXT_BYTES_MAX)
		v = (int)CLIP_TEXT_BYTES_MAX;
	a->settings_draft.max_clipboard_bytes = (size_t)v;
	uiLabelSetText(a->hotkey_hint_label,
		"Changed. Press Save to apply.");
}

static void on_persist_toggled(uiCheckbox *c, void *data)
{
	app_state *a = data;

	if (a == NULL)
		return;
	a->settings_draft.persist_history = uiCheckboxChecked(c);
	uiLabelSetText(a->hotkey_hint_label,
		"Changed. Press Save to apply.");
}

static void settings_reset_ui_from_draft(app_state *a)
{
	if (a == NULL)
		return;
	uiSpinboxSetValue(a->max_spin, (int)a->settings_draft.max_items);
	uiSpinboxSetValue(a->clip_bytes_spin,
	    (int)a->settings_draft.max_clipboard_bytes);
	uiCheckboxSetChecked(a->persist_checkbox,
	    a->settings_draft.persist_history);
	uiEntrySetText(a->hotkey_entry, a->settings_draft.hotkey);
}

static void on_settings_save(uiButton *b, void *data)
{
	app_state *a = data;
	size_t oldc;
	size_t newc;
	size_t i;
	int old_persist;

	(void)b;
	if (a == NULL || a->store == NULL || a->model == NULL)
		return;
	old_persist = a->cfg.persist_history;
	oldc = clip_store_count(a->store);
	clip_store_set_max_items(a->store, a->settings_draft.max_items);
	newc = clip_store_count(a->store);
	for (i = oldc; i > newc; i--)
		uiTableModelRowDeleted(a->model, (int)(i - 1));
	a->cfg = a->settings_draft;
	clip_store_set_max_clipboard_bytes(a->store, a->cfg.max_clipboard_bytes);
	if (old_persist && !a->cfg.persist_history) {
		if (a->state_path[0] != '\0')
			(void)clip_history_delete_file(a->state_path);
		a->history_dirty = 0;
	} else if (a->cfg.persist_history)
		app_mark_history_dirty(a);
	app_save_config(a);
	app_flush_history(a);
	{
		char reason[160];
		if (app_rebind_global_hotkey(a, reason, sizeof reason) == 0)
			uiLabelSetText(a->hotkey_hint_label,
				"Saved. Global hotkey is active.");
		else if (reason[0] != '\0')
			uiLabelSetText(a->hotkey_hint_label, reason);
		else
			uiLabelSetText(a->hotkey_hint_label,
				"Saved, but global hotkey is unavailable.");
	}
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
	uiGroup *hist_group;
	uiGroup *shortcut_group;
	uiForm *hist_form;
	uiForm *shortcut_form;

	if (a->settings_win != NULL)
		return;
	a->settings_win = uiNewWindow("Settings", 480, 320, 0);
	uiWindowSetMargined(a->settings_win, 1);

	root = uiNewVerticalBox();
	uiBoxSetPadded(root, 1);

	hist_group = uiNewGroup("History");
	hist_form = uiNewForm();
	uiFormSetPadded(hist_form, 1);
	a->max_spin = uiNewSpinbox((int)CLIP_STORE_MIN_ITEMS,
	    (int)CLIP_STORE_CAP_MAX_ITEMS);
	uiSpinboxSetValue(a->max_spin,
	    (int)clip_store_get_max_items(a->store));
	uiSpinboxOnChanged(a->max_spin, on_max_spin_changed, a);
	uiFormAppend(hist_form, "Maximum items", uiControl(a->max_spin), 0);
	a->clip_bytes_spin = uiNewSpinbox((int)CLIP_TEXT_BYTES_MIN,
	    (int)CLIP_TEXT_BYTES_MAX);
	uiSpinboxSetValue(a->clip_bytes_spin,
	    (int)clip_store_get_max_clipboard_bytes(a->store));
	uiSpinboxOnChanged(a->clip_bytes_spin, on_clip_bytes_spin_changed, a);
	uiFormAppend(hist_form, "Max text (UTF-8 bytes)", uiControl(a->clip_bytes_spin),
	    0);
	a->persist_checkbox = uiNewCheckbox("Save history to disk (history.bin)");
	uiCheckboxSetChecked(a->persist_checkbox, a->cfg.persist_history);
	uiCheckboxOnToggled(a->persist_checkbox, on_persist_toggled, a);
	uiFormAppend(hist_form, "", uiControl(a->persist_checkbox), 0);
	a->persist_hint_label = uiNewLabel(
	    "Tip: Off = RAM only, safer on shared machines.");
	uiFormAppend(hist_form, "", uiControl(a->persist_hint_label), 0);
	uiGroupSetChild(hist_group, uiControl(hist_form));
	uiBoxAppend(root, uiControl(hist_group), 0);

	uiBoxAppend(root, uiControl(uiNewHorizontalSeparator()), 0);

	shortcut_group = uiNewGroup("Shortcut");
	shortcut_form = uiNewForm();
	uiFormSetPadded(shortcut_form, 1);
	a->hotkey_entry = uiNewEntry();
	platform_bind_hotkey_entry(a->hotkey_entry, on_hotkey_capture_arm,
		on_hotkey_capture_commit, a);
	uiFormAppend(shortcut_form, "Global hotkey", uiControl(a->hotkey_entry), 0);
	uiFormAppend(shortcut_form, "",
	    uiControl(uiNewLabel("Focus field, press combo, then Save.")), 0);
	uiGroupSetChild(shortcut_group, uiControl(shortcut_form));
	uiBoxAppend(root, uiControl(shortcut_group), 0);

	uiBoxAppend(root, uiControl(uiNewHorizontalSeparator()), 0);

	a->hotkey_hint_label = uiNewLabel(" ");
	uiBoxAppend(root, uiControl(a->hotkey_hint_label), 0);

	actions = uiNewHorizontalBox();
	uiBoxSetPadded(actions, 1);
	uiBoxAppend(actions, uiControl(uiNewLabel("")), 1);
	a->settings_save_btn = uiNewButton("Save");
	uiButtonOnClicked(a->settings_save_btn, on_settings_save, a);
	uiBoxAppend(actions, uiControl(a->settings_save_btn), 0);
	a->settings_cancel_btn = uiNewButton("Cancel");
	uiButtonOnClicked(a->settings_cancel_btn, on_settings_cancel, a);
	uiBoxAppend(actions, uiControl(a->settings_cancel_btn), 0);
	uiBoxAppend(root, uiControl(actions), 0);

	uiWindowSetChild(a->settings_win, uiControl(root));
	uiWindowSetResizeable(a->settings_win, 0);
	uiWindowOnClosing(a->settings_win, on_settings_closing, a);
}

static void ensure_about_window(app_state *a)
{
	uiBox *root;
	uiLabel *label;
	char text[512];

	if (a->about_win != NULL)
		return;
	a->about_win = uiNewWindow("About", 320, 120, 0);
	uiWindowSetMargined(a->about_win, 1);
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
	root = uiNewVerticalBox();
	uiBoxSetPadded(root, 0);
	uiBoxAppend(root, uiControl(label), 0);
	uiWindowSetChild(a->about_win, uiControl(root));
	uiWindowSetResizeable(a->about_win, 0);
	uiWindowOnClosing(a->about_win, on_about_closing, a);
}

static void open_settings_window(app_state *a)
{
	if (a == NULL)
		return;
	a->settings_draft = a->cfg;
	ensure_settings_window(a);
	settings_reset_ui_from_draft(a);
	uiLabelSetText(a->hotkey_hint_label, "Save to apply changes.");
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
	app_mark_history_dirty(a);
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
	if (after > before) {
		uiTableModelRowInserted(a->model, 0);
		app_mark_history_dirty(a);
	}
}

static int on_clipboard_timer(void *data)
{
	(void)data;
	platform_clipboard_poll();
	return 1;
}

static int on_history_flush_timer(void *data)
{
	app_state *a = data;

	if (a != NULL)
		app_flush_history(a);
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
	const unsigned timer_ms = 200;
	uiMenu *mfile;
	uiMenu *medit;
	uiMenu *mhelp;
	uiMenuItem *prefs_item;
	uiMenuItem *clear_item;
	uiMenuItem *about_item;
	int want_picker;
	int sent_ipc;

	want_picker = 0;
	sent_ipc = 0;
	{
		int i;
		for (i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--picker") == 0)
				want_picker = 1;
		}
	}
	if (want_picker) {
		char reason[160];
		if (platform_ipc_request_picker(reason, sizeof reason) == 0)
			return 0;
		/* Fall through: no server; start normally and show picker. */
		sent_ipc = 0;
		(void)sent_ipc;
	}

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
	if (platform_user_state_path(g_app.state_path,
	    sizeof g_app.state_path) != 0)
		g_app.state_path[0] = '\0';
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
	clip_store_set_max_clipboard_bytes(g_app.store,
	    g_app.cfg.max_clipboard_bytes);
	if (g_app.cfg.persist_history && g_app.state_path[0] != '\0') {
		if (platform_ensure_state_parent(g_app.state_path) == 0)
			(void)clip_history_load(g_app.store, g_app.state_path);
	}

	g_app.model = uiNewTableModel(&g_mh);
	if (g_app.model == NULL) {
		clip_store_free(g_app.store);
		uiUninit();
		return 1;
	}
	{
		size_t k;
		size_t nhist;

		nhist = clip_store_count(g_app.store);
		for (k = 0; k < nhist; k++)
			uiTableModelRowInserted(g_app.model, 0);
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
	{
		char reason[160];
		if (platform_ipc_server_start(app_on_ipc_picker, &g_app,
		    reason, sizeof reason) != 0 && reason[0] != '\0')
			fprintf(stderr, "%s\n", reason);
	}
	{
		char reason[160];
		if (app_rebind_global_hotkey(&g_app, reason, sizeof reason) != 0 &&
		    reason[0] != '\0')
			fprintf(stderr, "%s\n", reason);
	}
	uiTimer(timer_ms, on_clipboard_timer, NULL);
	uiTimer(800, on_history_flush_timer, &g_app);

	if (!want_picker)
		uiControlShow(uiControl(w));
	if (want_picker)
		uiQueueMain(app_on_ipc_picker_queued, &g_app);
	uiMain();

	app_flush_history(&g_app);
	clip_store_free(g_app.store);
	g_app.store = NULL;
	uiFreeTableModel(g_app.model);
	g_app.model = NULL;
	uiUninit();
	return 0;
}
