/*
 * System clipboard: poll for changes, read/write plain text (UTF-8).
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include <ui.h>

/*
 * @param source_app Optional UTF-8 label for the app that owns the clipboard
 *                   (best-effort; NULL if unknown). Valid only for the duration
 *                   of the callback; clip_store must copy if retained.
 */
void platform_clipboard_init(void (*on_changed)(const char *utf8,
	const char *source_app, void *ud), void *userdata);
void platform_clipboard_shutdown(void);
void platform_clipboard_poll(void);

/* Returned pointer must be freed with free(). */
char *platform_clipboard_get_text(void);

/* Returns 0 on success, non-zero on failure. */
int platform_clipboard_set_text(const char *utf8);

/*
 * Tunes the main history list (e.g. column sizing, no horizontal scroll) for
 * the native toolkit. Safe to call once after the table columns are created.
 */
void platform_polish_clipboard_list(uiTable *table);

/*
 * Writes a UTF-8 path to the user config file (NUL-terminated). Returns 0 on
 * success, -1 if the path could not be determined.
 */
int platform_user_config_path(char *buf, size_t buf_size);

/*
 * Writes a UTF-8 path to the persistent history file (application state).
 * Separate from the config path per platform conventions. Returns 0 on success.
 */
int platform_user_state_path(char *buf, size_t buf_size);

/* Creates parent directories for the given file path. Returns 0 on success. */
int platform_ensure_config_parent(const char *utf8_path);

/* Same as platform_ensure_config_parent for the state file path. */
int platform_ensure_state_parent(const char *utf8_path);

typedef void (*platform_tray_show_cb)(void *userdata);
typedef void (*platform_tray_quit_cb)(void *userdata);

void platform_tray_init(uiWindow *main_win, platform_tray_show_cb show_cb,
	platform_tray_quit_cb quit_cb, void *userdata);
void platform_tray_shutdown(void);

typedef void (*platform_hotkey_arm_cb)(void *userdata);
typedef void (*platform_hotkey_commit_cb)(const char *hotkey, void *userdata);

/*
 * Best-effort native binding for "focus input then press combo" capture.
 * Returns 0 on success, non-zero if unsupported on this platform.
 */
int platform_bind_hotkey_entry(uiEntry *entry, platform_hotkey_arm_cb arm_cb,
	platform_hotkey_commit_cb commit_cb, void *userdata);

typedef void (*platform_global_hotkey_cb)(void *userdata);

/*
 * Register (or replace) a process-wide global hotkey.
 * Returns 0 on success, non-zero on failure/unsupported. If errbuf is
 * provided, it is filled with a short UTF-8 reason.
 */
int platform_global_hotkey_set(const char *hotkey,
	platform_global_hotkey_cb on_hotkey, void *userdata, char *errbuf,
	size_t errbuf_size);
void platform_global_hotkey_clear(void);

typedef struct platform_picker_item {
	const char *label;
	uint64_t token;
} platform_picker_item;

typedef void (*platform_picker_choose_cb)(uint64_t token, void *userdata);
typedef void (*platform_picker_cancel_cb)(void *userdata);

/*
 * Shows a quick picker near cursor (best-effort). Returns 0 if shown.
 * Non-zero means unsupported or failed.
 */
int platform_quick_picker_show(const platform_picker_item *items, size_t count,
	platform_picker_choose_cb choose_cb, platform_picker_cancel_cb cancel_cb,
	void *userdata);
void platform_quick_picker_hide(void);

/* Simulate the platform paste shortcut into currently focused app. */
int platform_simulate_paste(void);

typedef void (*platform_ipc_picker_cb)(void *userdata);

/*
 * Starts a lightweight IPC server for single-instance commands (best-effort).
 * Currently supports only: "picker" (show quick picker).
 *
 * Returns 0 on success; non-zero if unsupported or failed. If errbuf is
 * provided it is filled with a short UTF-8 reason.
 */
int platform_ipc_server_start(platform_ipc_picker_cb on_picker, void *userdata,
	char *errbuf, size_t errbuf_size);
void platform_ipc_server_stop(void);

/*
 * Requests an already-running instance to show the picker. Returns 0 if the
 * request was delivered. Non-zero means no server or unsupported.
 */
int platform_ipc_request_picker(char *errbuf, size_t errbuf_size);

#endif
