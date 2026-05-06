/*
 * Core clipboard history store (UI-agnostic).
 */

#ifndef CLIPBOARD_MANAGER_H
#define CLIPBOARD_MANAGER_H

#include <stddef.h>
#include <stdint.h>

typedef struct clip_item {
	uint64_t id;
	int64_t created_at_ms;
	char *text;
	char *source_app;
} clip_item;

typedef struct clip_store clip_store;

#define CLIP_APP_NAME "Clipboard manager"
#define CLIP_APP_VERSION "0.2.0"
#define CLIP_REPO_URL ""

#define CLIP_STORE_DEFAULT_MAX_ITEMS 100U
#define CLIP_STORE_MIN_ITEMS 10U
#define CLIP_STORE_CAP_MAX_ITEMS 5000U
#define CLIP_PREVIEW_MAX_BYTES 120
#define CLIP_HOTKEY_MAX 127

typedef struct clip_app_config {
	size_t max_items;
	char hotkey[CLIP_HOTKEY_MAX + 1];
} clip_app_config;

clip_store *clip_store_new(size_t max_items);
void clip_store_free(clip_store *s);

size_t clip_store_count(const clip_store *s);
size_t clip_store_get_max_items(const clip_store *s);
/*
 * Clamps max to [CLIP_STORE_MIN_ITEMS, CLIP_STORE_CAP_MAX_ITEMS]. Drops oldest
 * entries while count > max. Returns the new logical count (same as
 * clip_store_count after the call).
 */
size_t clip_store_set_max_items(clip_store *s, size_t max_items);

const clip_item *clip_store_get(const clip_store *s, size_t index);

/*
 * Inserts newest at index 0. Skips empty text, duplicates of the current newest
 * entry, and the first external clipboard sample matching a recent self-copy.
 * source_app may be NULL (stored as NULL).
 */
void clip_store_add_text(clip_store *s, const char *text, const char *source_app);

void clip_store_remove(clip_store *s, size_t index);
void clip_store_clear(clip_store *s);

/* Call after writing the same text to the system clipboard from this app. */
void clip_store_mark_self_copy(clip_store *s, const char *text);

/*
 * Writes a UTF-8 preview of item->text into buf (NUL-terminated, truncated).
 */
void clip_item_format_preview(const clip_item *item, char *buf, size_t buf_size);

int64_t clip_now_ms(void);

void clip_config_default(clip_app_config *cfg);
/*
 * Fills cfg with defaults, then overlays keys from path if the file exists.
 * Returns 0 if the file was read, -1 if missing or unreadable (defaults kept).
 */
int clip_config_load(const char *path, clip_app_config *cfg);
int clip_config_save(const char *path, const clip_app_config *cfg);

#endif
