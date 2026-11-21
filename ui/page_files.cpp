// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A document to send: ztOS has no system file picker, so a plain one. It opens on Downloads (else the home folder),
 * lists folders first and files by name with their size, goes into folders and back up to the home folder, never
 * above it. Hidden files are left out. Picking a file goes back to the chat and hands the path over.
 */
#include "wa.h"

#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

extern char** environ;

namespace wa {
namespace {

struct Entry {
	std::string name;
	bool b_dir = false;
	long size = 0;
};

std::function<void(const std::string&)> g_picked;
std::string g_dir;
Zid g_list = Z_INVALID_ID, g_path = Z_INVALID_ID, g_empty = Z_INVALID_ID;
std::vector<Entry> g_entries;

std::string home() {
	const char* h = getenv("HOME");
	return h ? h : "/";
}

void fill();

void go(const std::string& dir) {
	g_dir = dir;
	fill();
}

void fill() {
	if (!Zelement_is_valid(g_list)) return;
	Zitems_control_clear(g_list);
	g_entries.clear();
	const std::string h = home();
	if (g_dir != h) g_entries.push_back({"..", true, 0});
	std::vector<Entry> got;
	if (DIR* d = opendir(g_dir.c_str())) {
		while (dirent* e = readdir(d)) {
			const std::string n = e->d_name;
			if (n.empty() || n[0] == '.') continue;
			struct stat sb;
			if (stat((g_dir + "/" + n).c_str(), &sb) != 0) continue;
			if (!S_ISDIR(sb.st_mode) && !S_ISREG(sb.st_mode)) continue;
			got.push_back({n, S_ISDIR(sb.st_mode), long(sb.st_size)});
		}
		closedir(d);
	}
	std::sort(got.begin(), got.end(), [](const Entry& a, const Entry& b) {
		if (a.b_dir != b.b_dir) return a.b_dir;
		return lower(a.name) < lower(b.name);
	});
	g_entries.insert(g_entries.end(), got.begin(), got.end());
	std::string shown = g_dir == h ? "home" : g_dir.substr(h.size() + 1);
	Ztext_block_set_text(g_path, shown.c_str());
	for (const Entry& e : g_entries) {
		const Zid row = Zgrid_create();
		Zgrid_add_column(row, Z_GRID_AUTO, 0);
		Zgrid_add_column(row, Z_GRID_STAR, 1);
		Zelement_set_margin(row, Zthickness(0, H, 0, H));
		const Zid ic = icon_holder(e.b_dir ? symbol(Z_SYMBOL_FOLDER) : path_icon(glyph::page), 28, e.b_dir ? Z_RES_PHONE_ACCENT_BRUSH : Z_RES_PHONE_FOREGROUND_BRUSH);
		Zelement_set_vertical_alignment(ic, Z_VALIGN_CENTER);
		Zelement_set_margin(ic, Zthickness(0, 0, M, 0));
		Zgrid_place(row, ic, 0, 0);
		const Zid col = Zstack_panel_create(Z_VERTICAL);
		const Zid t = text(e.name == ".." ? "up" : e.name, Z_RES_TEXT_STYLE_LARGE_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_FOREGROUND_BRUSH);
		set_trim(t);
		Zpanel_add(col, t);
		if (!e.b_dir) Zpanel_add(col, text(human_size(e.size), Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR));
		Zelement_set_vertical_alignment(col, Z_VALIGN_CENTER);
		Zgrid_place(row, col, 1, 0);
		const Zid item = Zlist_view_item_create(nullptr);
		Zcontent_control_set_content(item, row);
		Zcontrol_set_horizontal_content_alignment(item, Z_HALIGN_STRETCH);
		Zitems_control_add(g_list, item);
	}
	Zelement_set_visibility(g_empty, got.empty() ? Z_VISIBLE : Z_COLLAPSED);
	if (Zscroll_viewer_get_vertical_offset(Zitems_control_get_scroll_viewer(g_list)) > 0)
		Zscroll_viewer_scroll_to_vertical_offset(Zitems_control_get_scroll_viewer(g_list), 0);
}

}  // namespace

std::string human_size(const long bytes) {
	char b[32];
	if (bytes < 1024) snprintf(b, sizeof b, "%ld B", bytes);
	else if (bytes < 1024 * 1024) snprintf(b, sizeof b, "%.0f kB", double(bytes) / 1024.0);
	else if (bytes < 1024L * 1024 * 1024) snprintf(b, sizeof b, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
	else snprintf(b, sizeof b, "%.1f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
	return b;
}

void open_external(const std::string& path) {
	const char* argv[] = {"xdg-open", path.c_str(), nullptr};
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	pid_t pid;
	if (posix_spawnp(&pid, "xdg-open", &fa, nullptr, const_cast<char* const*>(argv), environ) == 0)
		work([pid] { waitpid(pid, nullptr, 0); });
	posix_spawn_file_actions_destroy(&fa);
}

void pick_file(std::function<void(const std::string&)> picked) {
	g_picked = std::move(picked);
	struct stat sb;
	const std::string dl = home() + "/Downloads";
	g_dir = stat(dl.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode) ? dl : home();
	nav(page_pick_file, "", gap::slide_up());
}

Zid page_pick_file(const Zid, void*) {
	const Zid page = Zpage_create();
	Zid body;
	page_frame(page, "WHATSAPP", "send a document", &body);
	const Zid g = Zgrid_create();
	Zgrid_add_row(g, Z_GRID_AUTO, 0);
	Zgrid_add_row(g, Z_GRID_STAR, 1);
	Zpanel_add(body, g);
	g_path = text("", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_SEMIBOLD, Z_RES_PHONE_ACCENT_BRUSH);
	set_trim(g_path);
	Zelement_set_margin(g_path, Zthickness(0, 0, 0, G));
	Zgrid_place(g, g_path, 0, 0);
	g_list = Zlist_view_create();
	Zlist_view_set_selection_mode(g_list, Z_LIST_VIEW_SELECTION_NONE);
	Zlist_view_set_is_item_click_enabled(g_list, true);
	Zgrid_place(g, g_list, 0, 1);
	g_empty = empty_state("nothing here", "This folder has no files.");
	Zgrid_place(g, g_empty, 0, 1);
	Zlist_view_on_item_click(g_list, [](Zid, const int index, void*) {
		if (index < 0 || size_t(index) >= g_entries.size()) return;
		const Entry e = g_entries[size_t(index)];
		if (e.name == "..") {
			go(g_dir.substr(0, g_dir.rfind('/')));
			return;
		}
		if (e.b_dir) {
			go(g_dir + "/" + e.name);
			return;
		}
		const std::string path = g_dir + "/" + e.name;
		auto fn = g_picked;
		g_picked = nullptr;
		go_back();
		if (fn) post([fn, path] { fn(path); });
	}, nullptr);
	const Zid bar = app_bar();
	Zpage_set_bottom_app_bar(page, bar);
	bar_button(bar, "home", symbol(Z_SYMBOL_HOME), [] { go(home()); });
	bar_button(bar, "downloads", symbol(Z_SYMBOL_DOWNLOAD), [] {
		struct stat sb;
		const std::string dl = home() + "/Downloads";
		if (stat(dl.c_str(), &sb) == 0) go(dl);
	});
	bar_button(bar, "documents", symbol(Z_SYMBOL_DOCUMENT), [] {
		struct stat sb;
		const std::string d = home() + "/Documents";
		if (stat(d.c_str(), &sb) == 0) go(d);
	});
	fill();
	return page;
}

}  // namespace wa
