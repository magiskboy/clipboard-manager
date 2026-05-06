#include "platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#define IDI_APP_ICON 101

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

int platform_user_state_path(char *buf, size_t buf_size)
{
	char *lp;

	if (buf == NULL || buf_size == 0)
		return -1;
	lp = getenv("LOCALAPPDATA");
	if (lp == NULL || lp[0] == '\0')
		return -1;
	if (snprintf(buf, buf_size, "%s\\clipboard-manager\\history.bin", lp) >=
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

int platform_ensure_state_parent(const char *utf8_path)
{
	return platform_ensure_config_parent(utf8_path);
}

#define WM_CLIP_TRAY (WM_APP + 120)
#define WM_CLIP_PICKER (WM_APP + 121)
#define WM_CLIP_IPC_PICKER (WM_APP + 122)
#define CLIP_HOTKEY_ID 1
#define CLIP_DOUBLE_SHIFT_MS 350
#define CLIP_PIPE_NAME "\\\\.\\pipe\\clipboard-manager"

static HWND s_tray_hwnd;
static NOTIFYICONDATAW s_nid;
static int s_tray_inited;
static platform_tray_show_cb s_tray_show;
static platform_tray_quit_cb s_tray_quit;
static void *s_tray_ud;
static platform_global_hotkey_cb s_hotkey_cb;
static void *s_hotkey_ud;
static UINT s_hotkey_mods;
static UINT s_hotkey_vk;
static int s_hotkey_active;
static HHOOK s_keyboard_hook;
static DWORD s_last_shift_up_ms;
static platform_ipc_picker_cb s_ipc_picker_cb;
static void *s_ipc_ud;
static HANDLE s_ipc_thread;
static volatile LONG s_ipc_running;

static void write_err(char *buf, size_t buf_size, const char *msg)
{
	if (buf == NULL || buf_size == 0)
		return;
	snprintf(buf, buf_size, "%s", msg);
}

static int utf8_to_utf16(const char *s, wchar_t *buf, int cap)
{
	int n;

	if (s == NULL || buf == NULL || cap <= 0)
		return 0;
	n = MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, cap);
	if (n <= 0) {
		buf[0] = L'\0';
		return 0;
	}
	return n;
}

static UINT parse_hotkey_key(const char *name)
{
	size_t n;
	int fn;

	if (name == NULL || name[0] == '\0')
		return 0;
	n = strlen(name);
	if (n == 1) {
		unsigned char ch = (unsigned char)name[0];
		if (isalnum(ch))
			return (UINT)toupper(ch);
	}
	if (_stricmp(name, "Escape") == 0)
		return VK_ESCAPE;
	if (_stricmp(name, "Delete") == 0)
		return VK_DELETE;
	if (_stricmp(name, "Insert") == 0)
		return VK_INSERT;
	if (_stricmp(name, "Home") == 0)
		return VK_HOME;
	if (_stricmp(name, "End") == 0)
		return VK_END;
	if (_stricmp(name, "PageUp") == 0)
		return VK_PRIOR;
	if (_stricmp(name, "PageDown") == 0)
		return VK_NEXT;
	if (_stricmp(name, "Up") == 0)
		return VK_UP;
	if (_stricmp(name, "Down") == 0)
		return VK_DOWN;
	if (_stricmp(name, "Left") == 0)
		return VK_LEFT;
	if (_stricmp(name, "Right") == 0)
		return VK_RIGHT;
	if (_stricmp(name, "Tab") == 0)
		return VK_TAB;
	if (_stricmp(name, "Space") == 0)
		return VK_SPACE;
	if (_stricmp(name, "Enter") == 0)
		return VK_RETURN;
	if (name[0] == 'F' && name[1] != '\0') {
		fn = atoi(name + 1);
		if (fn >= 1 && fn <= 24)
			return (UINT)(VK_F1 + fn - 1);
	}
	return 0;
}

static int parse_hotkey_spec(const char *hotkey, UINT *mods_out, UINT *vk_out)
{
	char *copy;
	char *tok;
	UINT mods;
	UINT key;

	if (hotkey == NULL || hotkey[0] == '\0' || mods_out == NULL || vk_out == NULL)
		return -1;
	copy = strdup(hotkey);
	if (copy == NULL)
		return -1;
	mods = 0U;
	key = 0U;
	tok = strtok(copy, "+");
	while (tok != NULL) {
		if (_stricmp(tok, "Ctrl") == 0 || _stricmp(tok, "Control") == 0)
			mods |= MOD_CONTROL;
		else if (_stricmp(tok, "Alt") == 0)
			mods |= MOD_ALT;
		else if (_stricmp(tok, "Shift") == 0)
			mods |= MOD_SHIFT;
		else if (_stricmp(tok, "Super") == 0 || _stricmp(tok, "Win") == 0)
			mods |= MOD_WIN;
		else
			key = parse_hotkey_key(tok);
		tok = strtok(NULL, "+");
	}
	free(copy);
	if (key == 0U)
		return -1;
	*mods_out = mods;
	*vk_out = key;
	return 0;
}

static int get_picker_anchor(POINT *pt)
{
	HWND fg;
	DWORD tid;
	GUITHREADINFO gi;
	POINT p;

	if (pt == NULL)
		return -1;
	fg = GetForegroundWindow();
	if (fg == NULL)
		goto fallback_mouse;
	tid = GetWindowThreadProcessId(fg, NULL);
	if (tid == 0)
		goto fallback_mouse;
	memset(&gi, 0, sizeof gi);
	gi.cbSize = sizeof gi;
	if (!GetGUIThreadInfo(tid, &gi) || gi.hwndCaret == NULL)
		goto fallback_mouse;
	p.x = gi.rcCaret.left;
	p.y = gi.rcCaret.bottom;
	if (!ClientToScreen(gi.hwndCaret, &p))
		goto fallback_mouse;
	*pt = p;
	return 0;

fallback_mouse:
	if (!GetCursorPos(pt))
		return -1;
	return 0;
}

static void invoke_picker_hotkey(void)
{
	if (s_hotkey_cb != NULL)
		s_hotkey_cb(s_hotkey_ud);
}

static LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wparam, LPARAM lparam)
{
	KBDLLHOOKSTRUCT *k;
	DWORD now;

	if (code < 0)
		return CallNextHookEx(s_keyboard_hook, code, wparam, lparam);
	k = (KBDLLHOOKSTRUCT *)lparam;
	if (k == NULL)
		return CallNextHookEx(s_keyboard_hook, code, wparam, lparam);
	if (wparam == WM_KEYUP || wparam == WM_SYSKEYUP) {
		if (k->vkCode == VK_LSHIFT || k->vkCode == VK_RSHIFT) {
			now = GetTickCount();
			if (s_tray_hwnd != NULL && s_last_shift_up_ms != 0 &&
			    now - s_last_shift_up_ms <= CLIP_DOUBLE_SHIFT_MS)
				PostMessageW(s_tray_hwnd, WM_CLIP_PICKER, 0, 0);
			s_last_shift_up_ms = now;
		}
	}
	return CallNextHookEx(s_keyboard_hook, code, wparam, lparam);
}

static DWORD WINAPI ipc_server_thread(void *arg)
{
	HANDLE pipe;
	char buf[64];
	DWORD got;

	(void)arg;
	while (InterlockedCompareExchange(&s_ipc_running, 0, 0) != 0) {
		pipe = CreateNamedPipeA(CLIP_PIPE_NAME,
			PIPE_ACCESS_INBOUND,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
			1, 0, sizeof buf, 0, NULL);
		if (pipe == INVALID_HANDLE_VALUE)
			break;
		if (ConnectNamedPipe(pipe, NULL) ||
		    GetLastError() == ERROR_PIPE_CONNECTED) {
			got = 0;
			if (ReadFile(pipe, buf, sizeof buf - 1, &got, NULL) && got > 0) {
				buf[got] = '\0';
				if (strncmp(buf, "picker", 6) == 0 && s_tray_hwnd != NULL)
					PostMessageW(s_tray_hwnd, WM_CLIP_IPC_PICKER, 0, 0);
			}
		}
		DisconnectNamedPipe(pipe);
		CloseHandle(pipe);
	}
	return 0;
}

static LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam,
	LPARAM lparam)
{
	if (msg == WM_HOTKEY && (int)wparam == CLIP_HOTKEY_ID) {
		invoke_picker_hotkey();
		return 0;
	}
	if (msg == WM_CLIP_PICKER) {
		invoke_picker_hotkey();
		return 0;
	}
	if (msg == WM_CLIP_IPC_PICKER) {
		if (s_ipc_picker_cb != NULL)
			s_ipc_picker_cb(s_ipc_ud);
		return 0;
	}
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
	s_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL),
		MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
	if (s_nid.hIcon == NULL)
		s_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
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

int platform_global_hotkey_set(const char *hotkey,
	platform_global_hotkey_cb on_hotkey, void *userdata, char *errbuf,
	size_t errbuf_size)
{
	UINT mods;
	UINT vk;

	if (hotkey == NULL || hotkey[0] == '\0' || on_hotkey == NULL) {
		write_err(errbuf, errbuf_size, "Empty hotkey.");
		return -1;
	}
	if (parse_hotkey_spec(hotkey, &mods, &vk) != 0) {
		write_err(errbuf, errbuf_size, "Invalid hotkey format.");
		return -1;
	}
	tray_ensure_window();
	if (s_tray_hwnd == NULL) {
		write_err(errbuf, errbuf_size, "Hotkey host window unavailable.");
		return -1;
	}
	platform_global_hotkey_clear();
	if (!RegisterHotKey(s_tray_hwnd, CLIP_HOTKEY_ID, mods, vk)) {
		write_err(errbuf, errbuf_size,
			"Hotkey is already used by another application.");
		return -1;
	}
	s_hotkey_cb = on_hotkey;
	s_hotkey_ud = userdata;
	s_hotkey_mods = mods;
	s_hotkey_vk = vk;
	s_hotkey_active = 1;
	if (s_keyboard_hook == NULL)
		s_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL,
			keyboard_hook_proc, GetModuleHandleW(NULL), 0);
	write_err(errbuf, errbuf_size, "Global hotkey enabled.");
	return 0;
}

void platform_global_hotkey_clear(void)
{
	if (s_hotkey_active && s_tray_hwnd != NULL)
		UnregisterHotKey(s_tray_hwnd, CLIP_HOTKEY_ID);
	s_hotkey_cb = NULL;
	s_hotkey_ud = NULL;
	s_hotkey_mods = 0U;
	s_hotkey_vk = 0U;
	s_hotkey_active = 0;
	if (s_keyboard_hook != NULL) {
		UnhookWindowsHookEx(s_keyboard_hook);
		s_keyboard_hook = NULL;
	}
	s_last_shift_up_ms = 0;
}

int platform_quick_picker_show(const platform_picker_item *items, size_t count,
	platform_picker_choose_cb choose_cb, platform_picker_cancel_cb cancel_cb,
	void *userdata)
{
	HMENU menu;
	POINT pt;
	UINT id;
	wchar_t wbuf[256];
	uint64_t tokens[64];
	size_t n;
	size_t i;

	if (items == NULL || count == 0 || choose_cb == NULL)
		return -1;
	tray_ensure_window();
	if (s_tray_hwnd == NULL)
		return -1;
	menu = CreatePopupMenu();
	if (menu == NULL)
		return -1;
	n = count;
	if (n > sizeof tokens / sizeof tokens[0])
		n = sizeof tokens / sizeof tokens[0];
	for (i = 0; i < n; i++) {
		tokens[i] = items[i].token;
		if (!utf8_to_utf16(items[i].label != NULL ? items[i].label : "",
			wbuf, (int)(sizeof wbuf / sizeof wbuf[0]))) {
			wbuf[0] = L'\0';
		}
		AppendMenuW(menu, MF_STRING, (UINT)(1000 + i), wbuf);
	}
	if (get_picker_anchor(&pt) != 0) {
		pt.x = 120;
		pt.y = 120;
	}
	SetForegroundWindow(s_tray_hwnd);
	id = TrackPopupMenu(menu,
		TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
		pt.x + 8, pt.y + 8, 0, s_tray_hwnd, NULL);
	DestroyMenu(menu);
	if (id >= 1000 && id < 1000 + n) {
		choose_cb(tokens[id - 1000], userdata);
		return 0;
	}
	if (cancel_cb != NULL)
		cancel_cb(userdata);
	return 0;
}

void platform_quick_picker_hide(void)
{
}

int platform_simulate_paste(void)
{
	INPUT in[4];
	UINT sent;

	memset(in, 0, sizeof in);
	in[0].type = INPUT_KEYBOARD;
	in[0].ki.wVk = VK_CONTROL;
	in[1].type = INPUT_KEYBOARD;
	in[1].ki.wVk = 'V';
	in[2].type = INPUT_KEYBOARD;
	in[2].ki.wVk = 'V';
	in[2].ki.dwFlags = KEYEVENTF_KEYUP;
	in[3].type = INPUT_KEYBOARD;
	in[3].ki.wVk = VK_CONTROL;
	in[3].ki.dwFlags = KEYEVENTF_KEYUP;
	sent = SendInput((UINT)(sizeof in / sizeof in[0]), in, sizeof(INPUT));
	return sent == (UINT)(sizeof in / sizeof in[0]) ? 0 : -1;
}

int platform_ipc_server_start(platform_ipc_picker_cb on_picker, void *userdata,
	char *errbuf, size_t errbuf_size)
{
	if (on_picker == NULL) {
		write_err(errbuf, errbuf_size, "IPC picker callback missing.");
		return -1;
	}
	if (InterlockedCompareExchange(&s_ipc_running, 0, 0) != 0) {
		write_err(errbuf, errbuf_size, "IPC server already running.");
		return 0;
	}
	tray_ensure_window();
	s_ipc_picker_cb = on_picker;
	s_ipc_ud = userdata;
	InterlockedExchange(&s_ipc_running, 1);
	s_ipc_thread = CreateThread(NULL, 0, ipc_server_thread, NULL, 0, NULL);
	if (s_ipc_thread == NULL) {
		InterlockedExchange(&s_ipc_running, 0);
		write_err(errbuf, errbuf_size, "Failed to start IPC server thread.");
		return -1;
	}
	write_err(errbuf, errbuf_size, "IPC server started.");
	return 0;
}

void platform_ipc_server_stop(void)
{
	HANDLE h;
	DWORD wr;
	HANDLE c;
	const char *wake = "stop\n";

	InterlockedExchange(&s_ipc_running, 0);
	c = CreateFileA(CLIP_PIPE_NAME, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0,
		NULL);
	if (c != INVALID_HANDLE_VALUE) {
		(void)WriteFile(c, wake, (DWORD)strlen(wake), &wr, NULL);
		CloseHandle(c);
	}
	h = s_ipc_thread;
	if (h != NULL) {
		WaitForSingleObject(h, 1000);
		CloseHandle(h);
	}
	s_ipc_thread = NULL;
	s_ipc_picker_cb = NULL;
	s_ipc_ud = NULL;
}

int platform_ipc_request_picker(char *errbuf, size_t errbuf_size)
{
	HANDLE h;
	DWORD wr;
	const char *msg = "picker\n";

	h = CreateFileA(CLIP_PIPE_NAME, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0,
		NULL);
	if (h == INVALID_HANDLE_VALUE) {
		write_err(errbuf, errbuf_size,
			"No running instance (IPC connect failed).");
		return -1;
	}
	if (!WriteFile(h, msg, (DWORD)strlen(msg), &wr, NULL) || wr == 0) {
		CloseHandle(h);
		write_err(errbuf, errbuf_size, "Failed to send IPC request.");
		return -1;
	}
	CloseHandle(h);
	write_err(errbuf, errbuf_size, "Picker requested.");
	return 0;
}
