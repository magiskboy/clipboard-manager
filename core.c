#define _POSIX_C_SOURCE 200809L

#include "clipboard_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#define strcasecmp _stricmp
#else
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct clip_store {
	clip_item *items;
	size_t count;
	size_t capacity;
	uint64_t next_id;
	size_t max_items;
	size_t max_clipboard_bytes;
	char *suppress_text;
};

/* Magic CLIPHS01 — version 1 little-endian payload. */
static const char clip_hist_magic[8] = { 'C', 'L', 'I', 'P', 'H', 'S', '0', '1' };

static int parse_boolish(const char *s, int *out)
{
	if (s == NULL || out == NULL)
		return -1;
	if (strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0 ||
	    strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0) {
		*out = 1;
		return 0;
	}
	if (strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0 ||
	    strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0) {
		*out = 0;
		return 0;
	}
	return -1;
}

static size_t utf8_byte_count(const char *s, size_t max_bytes)
{
	size_t n;
	unsigned char c;
	size_t clen;

	if (s == NULL || max_bytes == 0)
		return 0;
	n = 0;
	while (s[n] != '\0' && n < max_bytes) {
		c = (unsigned char)s[n];
		if (c < 0x80)
			clen = 1;
		else if ((c & 0xE0) == 0xC0)
			clen = 2;
		else if ((c & 0xF0) == 0xE0)
			clen = 3;
		else if ((c & 0xF8) == 0xF0)
			clen = 4;
		else
			clen = 1;
		if (n + clen > max_bytes)
			break;
		n += clen;
	}
	return n;
}

static int wr_u8_buf(FILE *f, const void *p, size_t n)
{
	return fwrite(p, 1, n, f) == n ? 0 : -1;
}

static int wr_u32_le(FILE *f, uint32_t v)
{
	unsigned char b[4];

	b[0] = (unsigned char)(v & 0xff);
	b[1] = (unsigned char)((v >> 8) & 0xff);
	b[2] = (unsigned char)((v >> 16) & 0xff);
	b[3] = (unsigned char)((v >> 24) & 0xff);
	return wr_u8_buf(f, b, 4);
}

static int wr_u64_le(FILE *f, uint64_t v)
{
	unsigned char b[8];
	int i;

	for (i = 0; i < 8; i++) {
		b[i] = (unsigned char)(v & 0xff);
		v >>= 8;
	}
	return wr_u8_buf(f, b, 8);
}

static int wr_i64_le(FILE *f, int64_t v)
{
	return wr_u64_le(f, (uint64_t)v);
}

static int rd_u32_le(FILE *f, uint32_t *out)
{
	unsigned char b[4];

	if (fread(b, 1, 4, f) != 4)
		return -1;
	*out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	return 0;
}

static int rd_u64_le(FILE *f, uint64_t *out)
{
	unsigned char b[8];
	uint64_t v;
	int i;

	if (fread(b, 1, 8, f) != 8)
		return -1;
	v = 0;
	for (i = 0; i < 8; i++)
		v |= (uint64_t)b[i] << (8 * i);
	*out = v;
	return 0;
}

static int rd_i64_le(FILE *f, int64_t *out)
{
	uint64_t u;

	if (rd_u64_le(f, &u) != 0)
		return -1;
	*out = (int64_t)u;
	return 0;
}

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
	s->max_clipboard_bytes = CLIP_TEXT_BYTES_DEFAULT;
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
	if (s->items != NULL && s->count < s->capacity)
		memset(&s->items[s->count], 0, sizeof *s->items);
	return s->count;
}

void clip_store_set_max_clipboard_bytes(clip_store *s, size_t max_bytes)
{
	if (s == NULL)
		return;
	if (max_bytes < CLIP_TEXT_BYTES_MIN)
		max_bytes = CLIP_TEXT_BYTES_MIN;
	if (max_bytes > CLIP_TEXT_BYTES_MAX)
		max_bytes = CLIP_TEXT_BYTES_MAX;
	s->max_clipboard_bytes = max_bytes;
}

size_t clip_store_get_max_clipboard_bytes(const clip_store *s)
{
	if (s == NULL)
		return CLIP_TEXT_BYTES_DEFAULT;
	return s->max_clipboard_bytes;
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
	size_t tlen;
	char *tcopy;
	size_t cap;

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

	cap = s->max_clipboard_bytes;
	if (cap < CLIP_TEXT_BYTES_MIN)
		cap = CLIP_TEXT_BYTES_MIN;
	tlen = utf8_byte_count(text, cap);
	if (tlen == 0)
		return;

	tcopy = malloc(tlen + 1);
	if (tcopy == NULL)
		return;
	memcpy(tcopy, text, tlen);
	tcopy[tlen] = '\0';

	if (s->count > 0 && strcmp(s->items[0].text, tcopy) == 0) {
		free(tcopy);
		return;
	}

	memset(&newit, 0, sizeof newit);
	newit.id = s->next_id++;
	newit.created_at_ms = clip_now_ms();
	newit.text = tcopy;
	src = source_app;
	if (src != NULL && src[0] == '\0')
		src = NULL;
	if (src != NULL) {
		size_t slen;
		char *sc;

		slen = utf8_byte_count(src, 2048);
		if (slen == 0)
			src = NULL;
		else {
			sc = malloc(slen + 1);
			if (sc == NULL) {
				free(newit.text);
				return;
			}
			memcpy(sc, src, slen);
			sc[slen] = '\0';
			newit.source_app = sc;
		}
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
	if (s->items != NULL && s->count < s->capacity)
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
	cfg->max_clipboard_bytes = CLIP_TEXT_BYTES_DEFAULT;
	cfg->persist_history = 0;
	memcpy(cfg->hotkey, "Ctrl+Shift+V", sizeof "Ctrl+Shift+V");
}

static void clip_config_clamp(clip_app_config *cfg)
{
	if (cfg->max_items < CLIP_STORE_MIN_ITEMS)
		cfg->max_items = CLIP_STORE_MIN_ITEMS;
	if (cfg->max_items > CLIP_STORE_CAP_MAX_ITEMS)
		cfg->max_items = CLIP_STORE_CAP_MAX_ITEMS;
	if (cfg->max_clipboard_bytes < CLIP_TEXT_BYTES_MIN)
		cfg->max_clipboard_bytes = CLIP_TEXT_BYTES_MIN;
	if (cfg->max_clipboard_bytes > CLIP_TEXT_BYTES_MAX)
		cfg->max_clipboard_bytes = CLIP_TEXT_BYTES_MAX;
	cfg->persist_history = cfg->persist_history != 0;
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
		} else if (strcmp(p, "max_clipboard_bytes") == 0) {
			v = strtoul(eq, &end, 10);
			if (end != eq && *end == '\0' && v > 0)
				cfg->max_clipboard_bytes = (size_t)v;
		} else if (strcmp(p, "persist_history") == 0) {
			int b;

			if (parse_boolish(eq, &b) == 0)
				cfg->persist_history = b;
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
	    fprintf(f, "max_clipboard_bytes=%zu\n", tmp.max_clipboard_bytes) < 0 ||
	    fprintf(f, "persist_history=%d\n", tmp.persist_history) < 0 ||
	    fprintf(f, "hotkey=%s\n", tmp.hotkey) < 0) {
		fclose(f);
		return -1;
	}
	if (fclose(f) != 0)
		return -1;
	return 0;
}

#define CLIP_HIST_SOURCE_MAX 8192U

int clip_history_save(const clip_store *s, const char *path)
{
	FILE *f;
	size_t i;
	char tmp[768];
	const char *p;
	size_t plen;

	if (s == NULL || path == NULL || path[0] == '\0')
		return -1;
	p = path;
	plen = strlen(p);
	if (plen + 5 >= sizeof tmp)
		return -1;
	memcpy(tmp, p, plen);
	memcpy(tmp + plen, ".tmp", 5);
	f = fopen(tmp, "wb");
	if (f == NULL)
		return -1;
	if (wr_u8_buf(f, clip_hist_magic, 8) != 0 ||
	    wr_u32_le(f, 1) != 0 ||
	    wr_u64_le(f, s->next_id) != 0 ||
	    wr_u32_le(f, (uint32_t)s->count) != 0) {
		fclose(f);
		remove(tmp);
		return -1;
	}
	for (i = 0; i < s->count; i++) {
		const clip_item *it = &s->items[i];
		uint32_t tlen = it->text != NULL ? (uint32_t)strlen(it->text) : 0;
		uint32_t slen = it->source_app != NULL ?
		    (uint32_t)strlen(it->source_app) : 0;

		if (tlen > CLIP_TEXT_BYTES_MAX || slen > CLIP_HIST_SOURCE_MAX) {
			fclose(f);
			remove(tmp);
			return -1;
		}
		if (wr_u64_le(f, it->id) != 0 || wr_i64_le(f, it->created_at_ms) != 0 ||
		    wr_u32_le(f, tlen) != 0 || wr_u32_le(f, slen) != 0)
			goto fail;
		if (tlen > 0 && wr_u8_buf(f, it->text, tlen) != 0)
			goto fail;
		if (slen > 0 && wr_u8_buf(f, it->source_app, slen) != 0)
			goto fail;
	}
	if (fclose(f) != 0) {
		f = NULL;
		remove(tmp);
		return -1;
	}
	f = NULL;
#if !defined(_WIN32)
	if (chmod(tmp, (mode_t)(S_IRUSR | S_IWUSR)) != 0) {
		remove(tmp);
		return -1;
	}
#else
	(void)remove(path);
#endif
	if (rename(tmp, path) != 0) {
		remove(tmp);
		return -1;
	}
	return 0;
fail:
	if (f != NULL)
		fclose(f);
	remove(tmp);
	return -1;
}

int clip_history_load(clip_store *s, const char *path)
{
	FILE *f;
	uint32_t ver;
	uint32_t nfile;
	uint64_t file_next_id;
	uint32_t nkeep;
	uint32_t i = 0;
	uint32_t j;
	uint32_t free_upto;
	clip_item *newitems;
	size_t newcap;
	uint64_t max_seen_id;
	char m[8];

	if (s == NULL || path == NULL || path[0] == '\0')
		return -1;
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	if (fread(m, 1, 8, f) != 8 || memcmp(m, clip_hist_magic, 8) != 0)
		goto bad;
	if (rd_u32_le(f, &ver) != 0 || ver != 1 ||
	    rd_u64_le(f, &file_next_id) != 0 ||
	    rd_u32_le(f, &nfile) != 0)
		goto bad;
	if (nfile > CLIP_STORE_CAP_MAX_ITEMS)
		goto bad;
	nkeep = nfile;
	if (nkeep > s->max_items)
		nkeep = (uint32_t)s->max_items;
	if (nkeep == 0) {
		newitems = NULL;
		newcap = 0;
	} else {
		newitems = calloc((size_t)nkeep, sizeof *newitems);
		if (newitems == NULL)
			goto bad;
		newcap = (size_t)nkeep;
	}
	max_seen_id = 0;
	for (i = 0; i < nfile; i++) {
		uint64_t id;
		int64_t created;
		uint32_t tlen;
		uint32_t slen;
		char *tb = NULL;
		char *sb = NULL;

		if (rd_u64_le(f, &id) != 0 || rd_i64_le(f, &created) != 0 ||
		    rd_u32_le(f, &tlen) != 0 || rd_u32_le(f, &slen) != 0)
			goto bad2;
		if (tlen > CLIP_TEXT_BYTES_MAX || slen > CLIP_HIST_SOURCE_MAX)
			goto bad2;
		if (i < nkeep) {
			if (tlen > 0) {
				tb = malloc((size_t)tlen + 1);
				if (tb == NULL)
					goto bad2;
				if (fread(tb, 1, tlen, f) != tlen) {
					free(tb);
					goto bad2;
				}
				tb[tlen] = '\0';
			}
			if (slen > 0) {
				sb = malloc((size_t)slen + 1);
				if (sb == NULL) {
					free(tb);
					goto bad2;
				}
				if (fread(sb, 1, slen, f) != slen) {
					free(tb);
					free(sb);
					goto bad2;
				}
				sb[slen] = '\0';
			}
			newitems[i].id = id;
			newitems[i].created_at_ms = created;
			newitems[i].text = tb;
			newitems[i].source_app = sb;
			if (id > max_seen_id)
				max_seen_id = id;
		} else {
			if (fseek(f, (long)((size_t)tlen + (size_t)slen), SEEK_CUR) != 0)
				goto bad2;
		}
	}
	if (fgetc(f) != EOF)
		goto bad2;
	fclose(f);
	for (i = 0; i < s->count; i++)
		clip_item_free_fields(&s->items[i]);
	free(s->items);
	s->items = newitems;
	s->capacity = newcap;
	s->count = nkeep;
	if (file_next_id > max_seen_id + 1)
		s->next_id = file_next_id;
	else
		s->next_id = max_seen_id + 1;
	free(s->suppress_text);
	s->suppress_text = NULL;
	return 0;
bad2:
	free_upto = (i < nkeep) ? i : nkeep;
	if (newitems != NULL) {
		for (j = 0; j < free_upto; j++)
			clip_item_free_fields(&newitems[j]);
		free(newitems);
	}
	fclose(f);
	return -1;
bad:
	fclose(f);
	return -1;
}

int clip_history_delete_file(const char *path)
{
	if (path == NULL || path[0] == '\0')
		return -1;
	if (remove(path) != 0 && errno != ENOENT)
		return -1;
	return 0;
}
