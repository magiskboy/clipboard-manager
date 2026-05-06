#import <Cocoa/Cocoa.h>

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void (*g_on_changed)(const char *utf8, const char *source_app, void *ud);
static void *g_userdata;
static NSInteger g_last_change_count = -1;
static char *g_last_text_snapshot;

static char *copy_nsstring_utf8(NSString *s)
{
	const char *u;
	char *out;

	if (s == nil || [s length] == 0)
		return NULL;
	u = [s UTF8String];
	if (u == NULL)
		return NULL;
	out = strdup(u);
	return out;
}

static char *darwin_clipboard_source_app_utf8(void)
{
	@autoreleasepool {
		NSRunningApplication *app;
		NSString *n;

		app = [[NSWorkspace sharedWorkspace] frontmostApplication];
		if (app == nil)
			return NULL;
		n = [app localizedName];
		if (n == nil || [n length] == 0)
			return NULL;
		return copy_nsstring_utf8(n);
	}
}

void platform_clipboard_init(void (*on_changed)(const char *utf8,
	const char *source_app, void *ud), void *userdata)
{
	NSPasteboard *pb;

	g_on_changed = on_changed;
	g_userdata = userdata;
	free(g_last_text_snapshot);
	g_last_text_snapshot = NULL;

	@autoreleasepool {
		pb = [NSPasteboard generalPasteboard];
		g_last_change_count = [pb changeCount];
	}
}

void platform_clipboard_shutdown(void)
{
	g_on_changed = NULL;
	g_userdata = NULL;
	free(g_last_text_snapshot);
	g_last_text_snapshot = NULL;
	g_last_change_count = -1;
}

void platform_clipboard_poll(void)
{
	NSPasteboard *pb;
	NSInteger c;
	NSString *s;
	char *text;
	char *prev;
	char *src;

	@autoreleasepool {
		pb = [NSPasteboard generalPasteboard];
		c = [pb changeCount];
		if (c == g_last_change_count)
			return;
		g_last_change_count = c;

		s = [pb stringForType:NSPasteboardTypeString];
		text = copy_nsstring_utf8(s);
	}

	if (text == NULL)
		return;
	if (g_last_text_snapshot != NULL &&
	    strcmp(g_last_text_snapshot, text) == 0) {
		free(text);
		return;
	}
	prev = g_last_text_snapshot;
	g_last_text_snapshot = text;
	free(prev);

	src = darwin_clipboard_source_app_utf8();
	if (g_on_changed != NULL)
		g_on_changed(text, src, g_userdata);
	free(src);
}

char *platform_clipboard_get_text(void)
{
	@autoreleasepool {
		NSPasteboard *pb = [NSPasteboard generalPasteboard];
		NSString *s = [pb stringForType:NSPasteboardTypeString];
		return copy_nsstring_utf8(s);
	}
}

int platform_clipboard_set_text(const char *utf8)
{
	if (utf8 == NULL)
		return -1;
	@autoreleasepool {
		NSPasteboard *pb = [NSPasteboard generalPasteboard];
		NSString *s = [[NSString alloc] initWithUTF8String:utf8];
		if (s == nil)
			return -1;
		[pb clearContents];
		if ([pb setString:s forType:NSPasteboardTypeString])
			return 0;
		return -1;
	}
}

void platform_polish_clipboard_list(uiTable *table)
{
	(void)table;
}

int platform_user_config_path(char *buf, size_t buf_size)
{
	const char *home;

	if (buf == NULL || buf_size == 0)
		return -1;
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return -1;
	if (snprintf(buf, buf_size,
	    "%s/Library/Application Support/clipboard-manager/config.ini",
	    home) >= (int)buf_size)
		return -1;
	return 0;
}

int platform_ensure_config_parent(const char *utf8_path)
{
	NSString *path;
	NSString *dir;
	NSError *err;

	if (utf8_path == NULL || utf8_path[0] == '\0')
		return -1;
	path = [NSString stringWithUTF8String:utf8_path];
	if (path == nil)
		return -1;
	dir = [path stringByDeletingLastPathComponent];
	if ([dir length] == 0)
		return 0;
	if (![[NSFileManager defaultManager] createDirectoryAtPath:dir
	    withIntermediateDirectories:YES attributes:nil error:&err])
		return -1;
	return 0;
}

static platform_tray_show_cb s_darwin_show;
static platform_tray_quit_cb s_darwin_quit;
static void *s_darwin_ud;
static NSStatusItem *s_status_item;

@interface ClipboardTrayActions : NSObject
- (void)showWindow:(id)sender;
- (void)quitApp:(id)sender;
@end

@implementation ClipboardTrayActions
- (void)showWindow:(id)sender
{
	(void)sender;
	if (s_darwin_show != NULL)
		s_darwin_show(s_darwin_ud);
}
- (void)quitApp:(id)sender
{
	(void)sender;
	if (s_darwin_quit != NULL)
		s_darwin_quit(s_darwin_ud);
}
@end

static ClipboardTrayActions *s_tray_actions;

void platform_tray_init(uiWindow *main_win, platform_tray_show_cb show_cb,
	platform_tray_quit_cb quit_cb, void *userdata)
{
	NSMenu *menu;
	NSMenuItem *it;

	(void)main_win;
	platform_tray_shutdown();
	s_darwin_show = show_cb;
	s_darwin_quit = quit_cb;
	s_darwin_ud = userdata;
	@autoreleasepool {
		if (s_tray_actions == nil)
			s_tray_actions = [[ClipboardTrayActions alloc] init];
		s_status_item = [[NSStatusBar systemStatusBar]
		    statusItemWithLength:NSVariableStatusItemLength];
		[s_status_item setTitle:@"CB"];
		menu = [[NSMenu alloc] initWithTitle:@""];
		it = [[NSMenuItem alloc] initWithTitle:@"Show window"
		    action:@selector(showWindow:) keyEquivalent:@""];
		[it setTarget:s_tray_actions];
		[menu addItem:it];
		it = [[NSMenuItem alloc] initWithTitle:@"Quit"
		    action:@selector(quitApp:) keyEquivalent:@""];
		[it setTarget:s_tray_actions];
		[menu addItem:it];
		[s_status_item setMenu:menu];
	}
}

void platform_tray_shutdown(void)
{
	if (s_status_item != nil) {
		[[NSStatusBar systemStatusBar] removeStatusItem:s_status_item];
		s_status_item = nil;
	}
	s_darwin_show = NULL;
	s_darwin_quit = NULL;
	s_darwin_ud = NULL;
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
