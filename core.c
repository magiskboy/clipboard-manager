#define _POSIX_C_SOURCE 200809L

#include "clipboard_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

struct clip_store {
	clip_item *items;
	size_t count;
	size_t capacity;
	uint64_t next_id;
	size_t max_items;
	char *suppress_text;
};

int64_t clip_now_ms(void)
{
#if defined(_WIN32)
	FILETIME ft;
	ULARGE_INTEGER u;

	GetSystemTimeAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
#else
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return (int64_t)time(NULL) * 1000;
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void clip_item_free_fields(clip_item *it)
{
	free(it->text);
	free(it->source_app);
	it->text = NULL;
	it->source_app = NULL;
}

static int grow(clip_store *s)
{
	size_t ncap = s->capacity ? s->capacity * 2 : 8;
	clip_item *nitems;

	nitems = realloc(s->items, ncap * sizeof *nitems);
	if (nitems == NULL)
		return -1;
	s->items = nitems;
	s->capacity = ncap;
	return 0;
}

clip_store *clip_store_new(size_t max_items)
{
	clip_store *s;

	if (max_items == 0)
		max_items = CLIP_STORE_DEFAULT_MAX_ITEMS;
	s = calloc(1, sizeof *s);
	if (s == NULL)
		return NULL;
	s->max_items = max_items;
	s->next_id = 1;
	return s;
}

void clip_store_free(clip_store *s)
{
	size_t i;

	if (s == NULL)
		return;
	for (i = 0; i < s->count; i++)
		clip_item_free_fields(&s->items[i]);
	free(s->items);
	free(s->suppress_text);
	free(s);
}

size_t clip_store_count(const clip_store *s)
{
	return s != NULL ? s->count : 0;
}

size_t clip_store_get_max_items(const clip_store *s)
{
	if (s == NULL)
		return CLIP_STORE_DEFAULT_MAX_ITEMS;
	return s->max_items;
}

size_t clip_store_set_max_items(clip_store *s, size_t max_items)
{
	if (s == NULL)
		return 0;
	if (max_items < CLIP_STORE_MIN_ITEMS)
		max_items = CLIP_STORE_MIN_ITEMS;
	if (max_items > CLIP_STORE_CAP_MAX_ITEMS)
		max_items = CLIP_STORE_CAP_MAX_ITEMS;
	s->max_items = max_items;
	while (s->count > s->max_items) {
		clip_item_free_fields(&s->items[s->count - 1]);
		s->count--;
	}
	memset(&s->items[s->count], 0, sizeof *s->items);
	return s->count;
}

const clip_item *clip_store_get(const clip_store *s, size_t index)
{
	if (s == NULL || index >= s->count)
		return NULL;
	return &s->items[index];
}

void clip_store_mark_self_copy(clip_store *s, const char *text)
{
	char *copy;

	if (s == NULL || text == NULL)
		return;
	free(s->suppress_text);
	copy = strdup(text);
	s->suppress_text = copy;
}

void clip_store_add_text(clip_store *s, const char *text, const char *source_app)
{
	clip_item *slot;
	clip_item newit;
	const char *src;
	char *tsrc;

	if (s == NULL || text == NULL || text[0] == '\0')
		return;

	if (s->suppress_text != NULL) {
		if (strcmp(text, s->suppress_text) == 0) {
			free(s->suppress_text);
			s->suppress_text = NULL;
			return;
		}
		free(s->suppress_text);
		s->suppress_text = NULL;
	}

	if (s->count > 0 && strcmp(s->items[0].text, text) == 0)
		return;

	memset(&newit, 0, sizeof newit);
	newit.id = s->next_id++;
	newit.created_at_ms = clip_now_ms();
	newit.text = strdup(text);
	if (newit.text == NULL)
		return;
	src = source_app;
	if (src != NULL && src[0] == '\0')
		src = NULL;
	if (src != NULL) {
		tsrc = strdup(src);
		if (tsrc == NULL) {
			free(newit.text);
			return;
		}
		newit.source_app = tsrc;
	}

	if (s->count + 1 > s->capacity && grow(s) != 0) {
		clip_item_free_fields(&newit);
		return;
	}

	if (s->count >= s->max_items) {
		clip_item_free_fields(&s->items[s->count - 1]);
		s->count--;
	}

	memmove(s->items + 1, s->items, s->count * sizeof *s->items);
	slot = &s->items[0];
	*slot = newit;
	s->count++;
}

void clip_store_remove(clip_store *s, size_t index)
{
	size_t i;

	if (s == NULL || index >= s->count)
		return;
	clip_item_free_fields(&s->items[index]);
	for (i = index + 1; i < s->count; i++)
		s->items[i - 1] = s->items[i];
	s->count--;
	memset(&s->items[s->count], 0, sizeof *s->items);
}

void clip_store_clear(clip_store *s)
{
	size_t i;

	if (s == NULL)
		return;
	for (i = 0; i < s->count; i++)
		clip_item_free_fields(&s->items[i]);
	s->count = 0;
}

void clip_item_format_preview(const clip_item *item, char *buf, size_t buf_size)
{
	const char *t;
	size_t i, limit;

	if (buf == NULL || buf_size == 0)
		return;
	if (item == NULL || item->text == NULL) {
		buf[0] = '\0';
		return;
	}
	t = item->text;
	limit = buf_size - 1;
	if (limit > CLIP_PREVIEW_MAX_BYTES)
		limit = CLIP_PREVIEW_MAX_BYTES;
	for (i = 0; i < limit && t[i] != '\0'; i++) {
		if (t[i] == '\n' || t[i] == '\r')
			buf[i] = ' ';
		else
			buf[i] = t[i];
	}
	buf[i] = '\0';
	if (t[i] != '\0' && i + 3 < buf_size) {
		buf[i++] = '.';
		buf[i++] = '.';
		buf[i++] = '.';
		buf[i] = '\0';
	}
}

void clip_config_default(clip_app_config *cfg)
{
	if (cfg == NULL)
		return;
	cfg->max_items = CLIP_STORE_DEFAULT_MAX_ITEMS;
	memcpy(cfg->hotkey, "Ctrl+Shift+V", sizeof "Ctrl+Shift+V");
}

static void clip_config_clamp(clip_app_config *cfg)
{
	if (cfg->max_items < CLIP_STORE_MIN_ITEMS)
		cfg->max_items = CLIP_STORE_MIN_ITEMS;
	if (cfg->max_items > CLIP_STORE_CAP_MAX_ITEMS)
		cfg->max_items = CLIP_STORE_CAP_MAX_ITEMS;
	cfg->hotkey[CLIP_HOTKEY_MAX] = '\0';
}

static char *strip_line(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == '\n' || end[-1] == '\r' ||
	    end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	return s;
}

int clip_config_load(const char *path, clip_app_config *cfg)
{
	FILE *f;
	char line[256];
	char *p;
	char *eq;
	unsigned long v;
	char *end;

	clip_config_default(cfg);
	if (path == NULL || path[0] == '\0')
		return -1;
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof line, f) != NULL) {
		p = strip_line(line);
		if (p[0] == '\0' || p[0] == '#')
			continue;
		eq = strchr(p, '=');
		if (eq == NULL)
			continue;
		*eq++ = '\0';
		eq = strip_line(eq);
		p = strip_line(p);
		if (strcmp(p, "max_items") == 0) {
			v = strtoul(eq, &end, 10);
			if (end != eq && *end == '\0' && v > 0)
				cfg->max_items = (size_t)v;
		} else if (strcmp(p, "hotkey") == 0) {
			strncpy(cfg->hotkey, eq, CLIP_HOTKEY_MAX);
			cfg->hotkey[CLIP_HOTKEY_MAX] = '\0';
		}
	}
	fclose(f);
	clip_config_clamp(cfg);
	return 0;
}

int clip_config_save(const char *path, const clip_app_config *cfg)
{
	FILE *f;
	clip_app_config tmp;

	if (path == NULL || cfg == NULL)
		return -1;
	tmp = *cfg;
	clip_config_clamp(&tmp);
	f = fopen(path, "wb");
	if (f == NULL)
		return -1;
	if (fprintf(f, "max_items=%zu\n", tmp.max_items) < 0 ||
	    fprintf(f, "hotkey=%s\n", tmp.hotkey) < 0) {
		fclose(f);
		return -1;
	}
	if (fclose(f) != 0)
		return -1;
	return 0;
}
