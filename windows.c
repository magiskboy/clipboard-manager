#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shellapi.h>
#include <windows.h>

#if defined(_MSC_VER) && !defined(strdup)
#define strdup _strdup
#endif

static void (*g_on_changed)(const char *utf8, const char *source_app, void *ud);
static void *g_userdata;
static char *g_last_snapshot;

static char *utf16_to_utf8(const wchar_t *ws, size_t wlen)
{
	int n;
	char *out;

	if (ws == NULL || wlen == 0)
		return NULL;
	n = WideCharToMultiByte(CP_UTF8, 0, ws, (int)wlen, NULL, 0, NULL, NULL);
	if (n <= 0)
		return NULL;
	out = malloc((size_t)n + 1);
	if (out == NULL)
		return NULL;
	if (WideCharToMultiByte(CP_UTF8, 0, ws, (int)wlen, out, n, NULL, NULL) <= 0) {
		free(out);
		return NULL;
	}
	out[n] = '\0';
	return out;
}

static char *get_clipboard_utf8(void)
{
	HANDLE h;
	wchar_t *w;
	size_t wlen;
	char *out;

	if (!OpenClipboard(NULL))
		return NULL;
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
		CloseClipboard();
		return NULL;
	}
	h = GetClipboardData(CF_UNICODETEXT);
	if (h == NULL) {
		CloseClipboard();
		return NULL;
	}
	w = GlobalLock(h);
	if (w == NULL) {
		CloseClipboard();
		return NULL;
	}
	wlen = wcslen(w);
	out = utf16_to_utf8(w, wlen);
	GlobalUnlock(h);
	CloseClipboard();
	return out;
}

static char *windows_clipboard_owner_app_utf8(void)
{
	HWND ow;
	DWORD pid;
	HANDLE proc;
	DWORD n;
	WCHAR path[MAX_PATH];
	wchar_t *slash;
	size_t blen;
	WCHAR basebuf[MAX_PATH];
	const wchar_t *base;

	if (!OpenClipboard(NULL))
		return NULL;
	ow = GetClipboardOwner();
	pid = 0;
	if (ow != NULL)
		GetWindowThreadProcessId(ow, &pid);
	CloseClipboard();
	if (pid == 0)
		return NULL;
	proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (proc == NULL)
		return NULL;
	n = MAX_PATH;
	if (!QueryFullProcessImageNameW(proc, 0, path, &n)) {
		CloseHandle(proc);
		return NULL;
	}
	CloseHandle(proc);
	slash = wcsrchr(path, L'\\');
	if (slash != NULL)
		base = slash + 1;
	else
		base = path;
	blen = wcslen(base);
	if (blen > 4 && _wcsicmp(base + blen - 4, L".exe") == 0 &&
	    blen - 4 < MAX_PATH) {
		wcsncpy(basebuf, base, blen - 4);
		basebuf[blen - 4] = L'\0';
		return utf16_to_utf8(basebuf, wcslen(basebuf));
	}
	return utf16_to_utf8(base, blen);
}

void platform_clipboard_init(void (*on_changed)(const char *utf8,
	const char *source_app, void *ud), void *userdata)
{
	char *initial;

	g_on_changed = on_changed;
	g_userdata = userdata;
	free(g_last_snapshot);
	g_last_snapshot = NULL;

	initial = get_clipboard_utf8();
	if (initial != NULL) {
		g_last_snapshot = initial;
	} else {
		g_last_snapshot = NULL;
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
	char *t;
	char *src;

	t = get_clipboard_utf8();
	if (t == NULL)
		return;
	if (g_last_snapshot != NULL && strcmp(g_last_snapshot, t) == 0) {
		free(t);
		return;
	}
	free(g_last_snapshot);
	g_last_snapshot = t;
	src = windows_clipboard_owner_app_utf8();
	if (g_on_changed != NULL)
		g_on_changed(t, src, g_userdata);
	free(src);
}

char *platform_clipboard_get_text(void)
{
	return get_clipboard_utf8();
}

int platform_clipboard_set_text(const char *utf8)
{
	size_t len;
	int wlen;
	wchar_t *w;
	HANDLE h;
	wchar_t *lock;
	int r;

	if (utf8 == NULL)
		return -1;
	len = strlen(utf8);
	wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)(len + 1), NULL, 0);
	if (wlen <= 0)
		return -1;
	h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wlen * sizeof(wchar_t));
	if (h == NULL)
		return -1;
	lock = GlobalLock(h);
	if (lock == NULL) {
		GlobalFree(h);
		return -1;
	}
	r = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)(len + 1), lock, wlen);
	GlobalUnlock(h);
	if (r <= 0) {
		GlobalFree(h);
		return -1;
	}
	if (!OpenClipboard(NULL)) {
		GlobalFree(h);
		return -1;
	}
	EmptyClipboard();
	if (SetClipboardData(CF_UNICODETEXT, h) == NULL) {
		GlobalFree(h);
		CloseClipboard();
		return -1;
	}
	CloseClipboard();
	return 0;
}

void platform_polish_clipboard_list(uiTable *table)
{
	(void)table;
}

int platform_user_config_path(char *buf, size_t buf_size)
{
	char *ap;

	if (buf == NULL || buf_size == 0)
		return -1;
	ap = getenv("APPDATA");
	if (ap == NULL || ap[0] == '\0')
		return -1;
	if (snprintf(buf, buf_size, "%s\\clipboard-manager\\config.ini", ap) >=
	    (int)buf_size)
		return -1;
	return 0;
}

static int mkdir_p_win(char *path)
{
	char *p;
	int ok;

	for (p = path + 1; *p != '\0'; p++) {
		if (*p != '\\')
			continue;
		*p = '\0';
		ok = CreateDirectoryA(path, NULL);
		if (!ok && GetLastError() != ERROR_ALREADY_EXISTS) {
			*p = '\\';
			return -1;
		}
		*p = '\\';
	}
	ok = CreateDirectoryA(path, NULL);
	if (!ok && GetLastError() != ERROR_ALREADY_EXISTS)
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
	slash = strrchr(copy, '\\');
	if (slash == NULL)
		slash = strrchr(copy, '/');
	if (slash == NULL || slash == copy) {
		free(copy);
		return 0;
	}
	*slash = '\0';
	if (mkdir_p_win(copy) != 0) {
		free(copy);
		return -1;
	}
	free(copy);
	return 0;
}

#define WM_CLIP_TRAY (WM_APP + 120)

static HWND s_tray_hwnd;
static NOTIFYICONDATAW s_nid;
static int s_tray_inited;
static platform_tray_show_cb s_tray_show;
static platform_tray_quit_cb s_tray_quit;
static void *s_tray_ud;

static LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam,
	LPARAM lparam)
{
	if (msg == WM_CLIP_TRAY && s_tray_inited) {
		if (lparam == WM_LBUTTONDBLCLK || lparam == WM_LBUTTONUP) {
			if (s_tray_show != NULL)
				s_tray_show(s_tray_ud);
			return 0;
		}
		if (lparam == WM_RBUTTONUP) {
			HMENU menu;
			POINT pt;
			UINT id;

			menu = CreatePopupMenu();
			AppendMenuW(menu, MF_STRING, 1, L"Show window");
			AppendMenuW(menu, MF_STRING, 2, L"Quit");
			GetCursorPos(&pt);
			SetForegroundWindow(hwnd);
			id = TrackPopupMenu(menu,
				TPM_RIGHTALIGN | TPM_BOTTOMALIGN |
				TPM_RIGHTBUTTON | TPM_RETURNCMD,
				pt.x, pt.y, 0, hwnd, NULL);
			DestroyMenu(menu);
			if (id == 1 && s_tray_show != NULL)
				s_tray_show(s_tray_ud);
			if (id == 2 && s_tray_quit != NULL)
				s_tray_quit(s_tray_ud);
			return 0;
		}
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void tray_ensure_window(void)
{
	static int class_ready;
	WNDCLASSW wc;

	if (s_tray_hwnd != NULL)
		return;
	if (!class_ready) {
		memset(&wc, 0, sizeof wc);
		wc.lpfnWndProc = tray_wnd_proc;
		wc.hInstance = GetModuleHandleW(NULL);
		wc.lpszClassName = L"ClipMgrTrayHost";
		if (RegisterClassW(&wc) == 0 &&
		    GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return;
		class_ready = 1;
	}
	s_tray_hwnd = CreateWindowExW(0, L"ClipMgrTrayHost", NULL, 0, 0, 0, 0, 0,
		HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
}

void platform_tray_init(uiWindow *main_win, platform_tray_show_cb show_cb,
	platform_tray_quit_cb quit_cb, void *userdata)
{
	(void)main_win;

	platform_tray_shutdown();
	s_tray_show = show_cb;
	s_tray_quit = quit_cb;
	s_tray_ud = userdata;
	tray_ensure_window();
	if (s_tray_hwnd == NULL)
		return;
	memset(&s_nid, 0, sizeof s_nid);
	s_nid.cbSize = sizeof s_nid;
	s_nid.hWnd = s_tray_hwnd;
	s_nid.uID = 1;
	s_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	s_nid.uCallbackMessage = WM_CLIP_TRAY;
	s_nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
	wcscpy(s_nid.szTip, L"Clipboard manager");
	if (Shell_NotifyIconW(NIM_ADD, &s_nid))
		s_tray_inited = 1;
}

void platform_tray_shutdown(void)
{
	if (s_tray_inited) {
		Shell_NotifyIconW(NIM_DELETE, &s_nid);
		s_tray_inited = 0;
	}
	memset(&s_nid, 0, sizeof s_nid);
	if (s_tray_hwnd != NULL) {
		DestroyWindow(s_tray_hwnd);
		s_tray_hwnd = NULL;
	}
	s_tray_show = NULL;
	s_tray_quit = NULL;
	s_tray_ud = NULL;
}

int platform_bind_hotkey_entry(uiEntry *entry, platform_hotkey_arm_cb arm_cb,
	platform_hotkey_commit_cb commit_cb, void *userdata)
{
	(void)entry;
	(void)arm_cb;
	(void)commit_cb;
	(void)userdata;
	return -1;
}
