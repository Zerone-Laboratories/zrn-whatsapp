// SPDX-License-Identifier: GPL-2.0-or-later
/* S1 the main pivot (chats, status, channels), and the list pages S3, S10, S11, S12, S13 (spec 2.1, 2.4, 2.10). */
#include "wa.h"

#include <algorithm>
#include <ctime>

namespace wa {
namespace {

/* ---- the row of a chat (spec 2.1): built once, changed in place ---- */
struct Row {
	Zid container = Z_INVALID_ID, name = Z_INVALID_ID, time = Z_INVALID_ID, preview = Z_INVALID_ID, count = Z_INVALID_ID,
		count_text = Z_INVALID_ID, mute = Z_INVALID_ID, pin = Z_INVALID_ID, avatar = Z_INVALID_ID;
};

Zid row_grid(const std::string& jid, const std::string& name, const bool b_group, Row& r, const float avatar_border = 0,
	const ZThemeResource avatar_border_key = Z_RES_NONE) {
	/* The row of the Windows Phone app (wp:: in wa.h, docs/REFERENCE-MEASUREMENTS.md): a 49 point picture, 64 apart; the
	 * name's baseline 23.5 under the picture's top with the time's, the preview's and the count's 45.5 and 44.5 under it.
	 * The texts sit at fixed tops in one row (font line tops = baseline - 1.08 em), not centred in halves. */
	const Zid g = Zgrid_create();
	Zgrid_add_column(g, Z_GRID_PIXEL, wp::AVATAR);
	Zgrid_add_column(g, Z_GRID_STAR, 1);
	Zgrid_add_column(g, Z_GRID_AUTO, 0);
	Zgrid_add_row(g, Z_GRID_STAR, 1);
	Zgrid_add_row(g, Z_GRID_PIXEL, 0);
	const float pad = (wp::ROW_PITCH - wp::AVATAR) * 0.5f;
	Zelement_set_margin(g, Zthickness(0, pad, 0, pad));
	Zelement_set_height(g, wp::AVATAR);
	r.avatar = avatar(jid, name, wp::AVATAR, b_group);
	if (avatar_border > 0) {
		Zborder_bind_border_brush(r.avatar, avatar_border_key);
		Zborder_set_border_thickness(r.avatar, Zthickness_uniform(avatar_border));
	}
	Zgrid_place(g, r.avatar, 0, 0);
	Zgrid_set_row_span(r.avatar, 2);
	r.name = text_px(name, 26.0f, Z_FONT_SEMILIGHT, Z_RES_PHONE_FOREGROUND_BRUSH);
	set_trim(r.name);
	Zelement_set_margin(r.name, Zthickness(8.0f, 23.5f - 26.0f * 1.08f, 0, 0));
	Zelement_set_vertical_alignment(r.name, Z_VALIGN_TOP);
	Zgrid_place(g, r.name, 1, 0);
	r.time = text_px("", 16.0f, Z_FONT_NORMAL, Z_RES_PHONE_FOREGROUND_BRUSH);
	Zelement_set_margin(r.time, Zthickness(G, 23.5f - 16.0f * 1.08f, 0, 0));
	Zelement_set_horizontal_alignment(r.time, Z_HALIGN_RIGHT);
	Zelement_set_vertical_alignment(r.time, Z_VALIGN_TOP);
	Zgrid_place(g, r.time, 2, 0);
	r.preview = text_px("", 15.0f, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MID_COLOR);
	set_trim(r.preview);
	Zelement_set_margin(r.preview, Zthickness(8.0f, 45.5f - 15.0f * 1.08f, 0, 0));
	Zelement_set_vertical_alignment(r.preview, Z_VALIGN_TOP);
	Zgrid_place(g, r.preview, 1, 0);
	const Zid marks = Zstack_panel_create(Z_HORIZONTAL);
	Zelement_set_horizontal_alignment(marks, Z_HALIGN_RIGHT);
	Zelement_set_vertical_alignment(marks, Z_VALIGN_TOP);
	Zelement_set_margin(marks, Zthickness(0, 44.5f - 16.0f * 1.08f, 0, 0));
	Zgrid_place(g, marks, 2, 0);
	r.pin = icon_holder(path_icon(glyph::pin), 14, Z_RES_PHONE_BASE_MID_COLOR);
	Zelement_set_margin(r.pin, Zthickness(H, 4, 0, 0));
	Zelement_set_vertical_alignment(r.pin, Z_VALIGN_TOP);
	Zpanel_add(marks, r.pin);
	r.mute = icon_holder(path_icon(glyph::mute), 14, Z_RES_PHONE_BASE_MID_COLOR);
	Zelement_set_margin(r.mute, Zthickness(H, 4, 0, 0));
	Zelement_set_vertical_alignment(r.mute, Z_VALIGN_TOP);
	Zpanel_add(marks, r.mute);
	/* the unread count: accent text on the preview's line, no box (as the phone app) */
	r.count = border(Z_RES_NONE);
	Zelement_set_margin(r.count, Zthickness(H, 0, 0, 0));
	r.count_text = text_px("", 16.0f, Z_FONT_NORMAL, Z_RES_PHONE_ACCENT_BRUSH);
	Zelement_set_horizontal_alignment(r.count_text, Z_HALIGN_RIGHT);
	Zborder_set_child(r.count, r.count_text);
	Zpanel_add(marks, r.count);
	Zelement_set_visibility(r.pin, Z_COLLAPSED);
	Zelement_set_visibility(r.mute, Z_COLLAPSED);
	Zelement_set_visibility(r.count, Z_COLLAPSED);
	return g;
}

Zid add_row(const Zid list, const Zid content, const std::string& key) {
	const Zid c = Zlist_view_item_create(nullptr);
	Zcontent_control_set_content(c, content);
	Zcontrol_set_horizontal_content_alignment(c, Z_HALIGN_STRETCH);
	Zelement_set_name(c, key.c_str());
	gap::cache_bitmap(c);
	Zitems_control_add(list, c);
	return c;
}

std::string key_at(const Zid list, const int index) {
	return Zelement_get_name(Zitems_control_get_container(list, index));
}

Zid make_list() {
	const Zid l = Zlist_view_create();
	Zlist_view_set_selection_mode(l, Z_LIST_VIEW_SELECTION_NONE);
	Zlist_view_set_is_item_click_enabled(l, true);
	return l;
}

/* a list page: the list, its empty state and the top progress bar in one cell */
struct ListBox {
	Zid cell, list, empty, progress;
};
ListBox list_box(const char* heading, const char* line) {
	ListBox b;
	b.cell = Zgrid_create();
	b.list = make_list();
	Zpanel_add(b.cell, b.list);
	b.empty = empty_state(heading, line);
	Zelement_set_vertical_alignment(b.empty, Z_VALIGN_TOP);
	Zelement_set_visibility(b.empty, Z_COLLAPSED);
	Zpanel_add(b.cell, b.empty);
	b.progress = top_progress();
	Zpanel_add(b.cell, b.progress);
	return b;
}

/* ---- S1 ---- */
struct Main {
	Zid page = Z_INVALID_ID, pivot, bar, badge = Z_INVALID_ID, badge_count = Z_INVALID_ID;
	ListBox chats, status, channels;
	std::map<std::string, Row> rows;
	std::vector<int> status_map; /* item index -> person, -1 my status */
	bool b_shown[3] = {false, false, false};
	/* the chats came after the page was in: they swing in once, as the rows of a page do (spec 4.3) */
	bool b_chats_entered = false;
} m;

bool main_alive() { return m.page != Z_INVALID_ID && Zelement_is_valid(m.page); }

/* the title's badge: how many chats have unread messages (hidden at none) */
void badge_update() {
	if (!Zelement_is_valid(m.badge)) return;
	int n = 0;
	for (const auto& kv : st.chats)
		if (kv.second.unread > 0 && !kv.second.b_archived) n++;
	Zelement_set_visibility(m.badge, n > 0 ? Z_VISIBLE : Z_COLLAPSED);
	Ztext_block_set_text(m.badge_count, std::to_string(n).c_str());
}

void chat_row_fill(const Chat& c, Row& r) {
	Ztext_block_set_text(r.name, c.name.c_str());
	Ztext_block_set_text(r.time, time_short(c.last_ts).c_str());
	const bool b_unread = c.unread > 0;
	const bool b_typing = st.typing.count(c.id) && st.typing[c.id];
	std::string pv = b_typing ? "typing..." : (c.b_last_from_me ? "you: " : "") + one_line(c.last_text);
	Ztext_block_set_text(r.preview, pv.c_str());
	/* unread: the preview in the accent with the count; read: subtle */
	Ztext_block_bind_foreground(r.preview, b_typing || b_unread ? Z_RES_PHONE_ACCENT_BRUSH : Z_RES_PHONE_BASE_MID_COLOR);
	Zelement_set_visibility(r.count, b_unread ? Z_VISIBLE : Z_COLLAPSED);
	Ztext_block_set_text(r.count_text, c.unread > 999 ? "999+" : std::to_string(c.unread).c_str());
	Zelement_set_visibility(r.mute, c.b_muted ? Z_VISIBLE : Z_COLLAPSED);
	Zelement_set_visibility(r.pin, c.b_pinned ? Z_VISIBLE : Z_COLLAPSED);
	badge_update();
}

void chat_menu(const std::string& id);

void chats_enter();

/* The rows a screen holds are built at once; the rest come in slices when no animation runs (post_idle), so a list of
 * hundreds of chats is not built in the frame that turns the page in. A newer rebuild drops the slices of an older one. */
constexpr size_t ROWS_FIRST = 14, ROWS_SLICE = 12;
unsigned g_rows_gen;
bool g_rows_building; /* slices of the last rebuild are still to come */

void chat_row_build(const Chat& c) {
	Row r;
	const Zid g = row_grid(c.id, c.name, c.b_group, r);
	r.container = add_row(m.chats.list, g, c.id);
	chat_row_fill(c, r);
	gap::on_holding(r.container, [](Zid sender, void*) { chat_menu(Zelement_get_name(sender)); }, nullptr);
	m.rows[c.id] = r;
}

void chats_rest(const unsigned gen, std::shared_ptr<std::vector<std::string>> ids, const size_t at) {
	post_idle([gen, ids, at] {
		if (gen != g_rows_gen || !main_alive()) return;
		const size_t end = std::min(at + ROWS_SLICE, ids->size());
		for (size_t i = at; i < end; i++) {
			auto it = st.chats.find((*ids)[i]);
			if (it == st.chats.end() || it->second.b_archived || m.rows.count(it->first)) continue;
			chat_row_build(it->second);
		}
		if (end < ids->size()) chats_rest(gen, ids, end);
		else g_rows_building = false;
	});
}

void chats_rebuild() {
	Zitems_control_clear(m.chats.list);
	m.rows.clear();
	const unsigned gen = ++g_rows_gen;
	auto ids = std::make_shared<std::vector<std::string>>();
	for (const Chat* c : chats_sorted(false)) {
		if (m.rows.size() < ROWS_FIRST) chat_row_build(*c);
		else ids->push_back(c->id);
	}
	g_rows_building = !ids->empty();
	if (g_rows_building) chats_rest(gen, ids, 0);
	Zelement_set_visibility(m.chats.progress, st.b_chats_loaded || !engine_up() ? Z_COLLAPSED : Z_VISIBLE);
	Zelement_set_visibility(m.chats.empty, st.b_chats_loaded && m.rows.empty() ? Z_VISIBLE : Z_COLLAPSED);
	chats_enter();
}

/* the first chats in a page already shown fade in (a second turnstile right after the page's own read as the start played
 * twice); joining a page still turning in, they are in its turn */
void chats_enter() {
	if (!main_alive() || m.b_chats_entered || m.rows.empty() || !m.b_shown[0]) return;
	m.b_chats_entered = true;
	if (Zframe_is_transitioning(frame()) || Zpivot_get_selected_index(m.pivot) != 0) return;
	Zelement_set_opacity(m.chats.list, 0);
	Zelement_animate(m.chats.list, Z_PROP_OPACITY, 1, 150, Z_EASE_OUT, 0);
}

/* G2 (long press) only: pin is the one app-state change the engine makes; the rest is read-only there (IPC.md) */
void chat_menu(const std::string& id) {
	auto it = st.chats.find(id);
	auto r = m.rows.find(id);
	if (it == st.chats.end() || r == m.rows.end()) return;
	const Zid menu = Zmenu_flyout_create();
	const bool b_pinned = it->second.b_pinned;
	menu_item(menu, b_pinned ? "unpin" : "pin to top", [id, b_pinned] {
		request("pin_chat", {{"chat", id}, {"pinned", !b_pinned}}, [](const json*, const char* err) { if (!err) load_chats(); });
	});
	menu_item(menu, it->second.b_muted ? "unmute" : "mute", [] {}, false);
	menu_item(menu, "archive", [] {}, false);
	menu_item(menu, "mark as read", [id] { request("mark_read", {{"chat", id}}); }, it->second.unread > 0);
	menu_item(menu, "delete chat", [] {}, false);
	Zflyout_base_show_at(menu, r->second.container);
}

void status_rebuild() {
	Zitems_control_clear(m.status.list);
	m.status_map.clear();
	Row me;
	const Zid g = row_grid(st.self_jid, "me", false, me);
	Ztext_block_set_text(me.name, "add status");
	Ztext_block_set_text(me.preview, "tap to add a status update");
	add_row(m.status.list, g, "");
	m.status_map.push_back(-1);
	for (int pass = 0; pass < 2; pass++) {
		bool b_header = false;
		for (size_t i = 0; i < st.statuses.size(); i++) {
			const StatusPerson& p = st.statuses[i];
			if ((p.unseen > 0) != (pass == 0)) continue;
			if (!b_header) {
				Zlist_view_add_group_header(m.status.list, pass == 0 ? "recent" : "viewed");
				b_header = true;
			}
			Row r;
			const Zid rg = row_grid(p.sender, p.name, false, r, 3, p.unseen ? Z_RES_PHONE_ACCENT_BRUSH : Z_RES_PHONE_BASE_LOW_COLOR);
			Ztext_block_set_text(r.time, time_short(p.last_ts).c_str());
			Ztext_block_set_text(r.preview, (std::to_string(p.items.size()) + (p.items.size() == 1 ? " update" : " updates")).c_str());
			add_row(m.status.list, rg, p.sender);
			m.status_map.push_back(int(i));
		}
	}
	Zelement_set_visibility(m.status.progress, st.b_statuses_loaded || !engine_up() ? Z_COLLAPSED : Z_VISIBLE);
	Zelement_set_visibility(m.status.empty, st.b_statuses_loaded && st.statuses.empty() ? Z_VISIBLE : Z_COLLAPSED);
}

void channels_rebuild() {
	Zitems_control_clear(m.channels.list);
	for (const Channel& c : st.channels) {
		Row r;
		const Zid g = row_grid(c.id, c.name, false, r);
		Ztext_block_set_text(r.preview, one_line(c.about).c_str());
		add_row(m.channels.list, g, c.id);
	}
	Zelement_set_visibility(m.channels.progress, st.b_channels_loaded || !engine_up() ? Z_COLLAPSED : Z_VISIBLE);
	Zelement_set_visibility(m.channels.empty, st.b_channels_loaded && st.channels.empty() ? Z_VISIBLE : Z_COLLAPSED);
}

void bar_for(const int index) {
	Zcommand_bar_clear_primary_commands(m.bar);
	Zcommand_bar_clear_secondary_commands(m.bar);
	if (index == 0) {
		/* search, then new, as the phone app */
		bar_button(m.bar, "search", symbol(Z_SYMBOL_FIND), [] { nav(page_search); });
		bar_button(m.bar, "new", symbol(Z_SYMBOL_ADD), [] { nav(page_new_chat); });
		bar_secondary(m.bar, "starred messages", [] { nav(page_starred); });
		bar_secondary(m.bar, "archived chats", [] { nav(page_archived); });
	} else if (index == 1) {
		bar_button(m.bar, "new", symbol(Z_SYMBOL_ADD), [] { nav(page_new_status); });
	} else {
		bar_button(m.bar, "search", symbol(Z_SYMBOL_FIND), [] { nav(page_search); });
		bar_button(m.bar, "follow", symbol(Z_SYMBOL_ADD), [] { nav(page_follow); });
	}
	bar_secondary(m.bar, "settings", [] { nav(page_settings); });
	if (index == 0) bar_secondary(m.bar, "account", [] { nav(page_account); });
	bar_enable(m.bar, engine_up());
}

void pivot_item_shown(const int index) {
	bar_for(index);
	if (index < 0 || index > 2 || m.b_shown[index]) return;
	m.b_shown[index] = true;
	/* rows that are there now come in with the page or the entrance below; only chats that come later need chats_enter */
	if (index == 0 && !m.rows.empty()) m.b_chats_entered = true;
	/* the page's own turnstile already swings these rows in: a second entrance on the same rows fights it (flicker) */
	if (Zframe_is_transitioning(frame())) return;
	list_entrance(index == 0 ? m.chats.list : index == 1 ? m.status.list : m.channels.list);
}

}  // namespace

Zid page_main(const Zid, void*) {
	m = Main();
	m.page = Zpage_create();
	Zpage_set_navigation_cache_mode(m.page, Z_NAVIGATION_CACHE_REQUIRED);
	const Zid g = Zgrid_create();
	Zpage_set_content(m.page, g);
	m.pivot = Zpivot_create();
	/* the title is the page's own (Bold, with the unread badge after it); the pivot keeps only the strip (wp:: in wa.h) */
	Zpanel_add(g, m.pivot);
	{
		const Zid row = Zstack_panel_create(Z_HORIZONTAL);
		Zelement_set_margin(row, Zthickness(wp::LEFT, wp::TITLE_TOP, M, 0));
		Zelement_set_vertical_alignment(row, Z_VALIGN_TOP);
		Zelement_set_horizontal_alignment(row, Z_HALIGN_LEFT);
		Zelement_set_hit_test_visible(row, false);
		const Zid t = text_px("WHATSAPP", wp::TITLE_SIZE, Z_FONT_BOLD, Z_RES_PHONE_FOREGROUND_BRUSH);
		Zpanel_add(row, t);
		m.badge = Zstack_panel_create(Z_HORIZONTAL);
		Zelement_set_margin(m.badge, Zthickness(11.5f, 0, 0, 0));
		const Zid bubble = icon_holder(path_icon(glyph::chat_badge), 13, Z_RES_PHONE_ACCENT_BRUSH);
		Zelement_set_vertical_alignment(bubble, Z_VALIGN_TOP);
		Zelement_set_margin(bubble, Zthickness(0, 5.5f, 0, 0));
		Zpanel_add(m.badge, bubble);
		m.badge_count = text_px("", 12.5f, Z_FONT_SEMIBOLD, Z_RES_PHONE_ACCENT_BRUSH);
		Zelement_set_margin(m.badge_count, Zthickness(3.5f, 4.0f, 0, 0));
		Zelement_set_vertical_alignment(m.badge_count, Z_VALIGN_TOP);
		Zpanel_add(m.badge, m.badge_count);
		Zelement_set_visibility(m.badge, Z_COLLAPSED);
		Zpanel_add(row, m.badge);
		Zpanel_add(g, row);
	}
	const Zid bn = banner();
	Zelement_set_margin(bn, Zthickness(0, wp::STRIP_TOP + wp::HEADER_SIZE * 1.331f + wp::ITEM_TOP, 0, 0));
	Zpanel_add(g, bn);
	Zpanel_set_zindex(bn, 10);

	m.chats = list_box("no chats yet", "tap new to start one");
	m.status = list_box("no status updates", "");
	m.channels = list_box("you follow no channels", "");
	const Zid follow = Zhyperlink_button_create("follow a channel");
	Zcontrol_bind_font_size(follow, Z_RES_CONTENT_CONTROL_FONT_SIZE);
	Zelement_set_margin(follow, Zthickness(0, G, 0, M));
	click(follow, [] { nav(page_follow); });
	Zlist_view_set_footer_content(m.channels.list, follow);
	const char* heads[3] = {"chats", "status", "channels"};
	const Zid cells[3] = {m.chats.cell, m.status.cell, m.channels.cell};
	for (int i = 0; i < 3; i++) {
		const Zid it = Zpivot_item_create(heads[i]);
		Zelement_set_margin(it, Zthickness(wp::LEFT, wp::ITEM_TOP, wp::RIGHT, 0));
		Zcontent_control_set_content(it, cells[i]);
		Zpivot_add_item(m.pivot, it);
	}
	Zlist_view_on_item_click(m.chats.list, [](const Zid list, const int index, void*) {
		const std::string id = key_at(list, index);
		auto r = m.rows.find(id);
		if (r != m.rows.end()) gap::continuum(frame(), r->second.name);
		open_chat(id);
	}, nullptr);
	Zlist_view_on_item_click(m.status.list, [](Zid, const int index, void*) {
		if (index < 0 || size_t(index) >= m.status_map.size()) return;
		if (m.status_map[size_t(index)] < 0) {
			nav(page_new_status);
			return;
		}
		g_status_person = m.status_map[size_t(index)];
		nav(page_status_view, "", gap::slide_up());
	}, nullptr);
	Zlist_view_on_item_click(m.channels.list, [](const Zid list, const int index, void*) {
		const std::string id = key_at(list, index);
		for (const Channel& c : st.channels)
			if (c.id == id) open_chat(c.id, c.name, true);
	}, nullptr);
	Zpivot_on_selection_changed(m.pivot, [](const Zid pivot, void*) { pivot_item_shown(Zpivot_get_selected_index(pivot)); }, nullptr);

	m.bar = app_bar();
	Zpage_set_bottom_app_bar(m.page, m.bar);
	chats_rebuild();
	status_rebuild();
	channels_rebuild();
	bar_for(0);
	Zpage_on_navigated_to(m.page, [](Zid, void*) {
		pivot_item_shown(Zpivot_get_selected_index(m.pivot));
		if (engine_up() && !st.b_chats_loaded) load_chats();
	}, nullptr);
	if (engine_up()) {
		if (!st.b_statuses_loaded) load_statuses();
		if (!st.b_channels_loaded) load_channels();
	}
	return m.page;
}

/* Spec 4.10: a chat that moves goes to its new place and fades in from 0.4; the others jump. */
void main_chats_changed(const std::string& id) {
	if (!main_alive()) return;
	auto it = st.chats.find(id);
	auto r = m.rows.find(id);
	/* a row not built yet while the slices still come: the slice builds it from st.chats as it is then */
	if (!id.empty() && r == m.rows.end() && g_rows_building && it != st.chats.end()) return;
	if (id.empty() || it == st.chats.end() || r == m.rows.end() || it->second.b_archived) {
		chats_rebuild();
		return;
	}
	chat_row_fill(it->second, r->second);
	const auto order = chats_sorted(false);
	int want = 0;
	for (; want < int(order.size()); want++)
		if (order[size_t(want)]->id == id) break;
	const int now = Zitems_control_index_from_container(m.chats.list, r->second.container);
	const Zid panel = Zitems_control_get_items_panel(m.chats.list);
	/* (while the rest of the rows are still being built, the place is at most the end of the ones there) */
	want = std::min(want, Zpanel_get_count(panel) - 1);
	if (now == want || now < 0) return;
	Zpanel_remove(panel, r->second.container);
	Zpanel_insert(panel, want, r->second.container);
	Zelement_set_opacity(r->second.container, 0.4f);
	Zelement_animate(r->second.container, Z_PROP_OPACITY, 1, 200, Z_EASE_OUT, 0);
}

void main_statuses_changed() {
	if (main_alive()) status_rebuild();
}
void main_channels_changed() {
	if (main_alive()) channels_rebuild();
}
void main_engine_changed() {
	if (!main_alive()) return;
	bar_enable(m.bar, engine_up());
	Zelement_set_visibility(m.chats.progress, st.b_chats_loaded || !engine_up() ? Z_COLLAPSED : Z_VISIBLE);
}

/* ======== S3 new chat (spec 2.4) ======== */
namespace {
struct NewChat {
	Zid page = Z_INVALID_ID, box, list, empty, progress;
	std::vector<std::string> keys;
} nc;

void new_chat_fill() {
	if (nc.page == Z_INVALID_ID || !Zelement_is_valid(nc.page)) return;
	Zitems_control_clear(nc.list);
	nc.keys.clear();
	const std::string q = lower(Ztext_box_get_text(nc.box));
	std::string digits;
	for (const char c : q)
		if (isdigit(static_cast<unsigned char>(c))) digits += c;
	bool b_match = false;
	std::vector<const Contact*> shown;
	for (const Contact& c : st.contacts)
		if (q.empty() || lower(c.name).find(q) != std::string::npos || (!digits.empty() && c.phone.find(digits) != std::string::npos))
			shown.push_back(&c);
	b_match = !shown.empty();
	if (digits.size() >= 7 && !b_match) {
		const Zid g = Zgrid_create();
		Zgrid_add_column(g, Z_GRID_PIXEL, 58);
		Zgrid_add_column(g, Z_GRID_STAR, 1);
		Zelement_set_margin(g, Zthickness(0, G, 0, G));
		const Zid av = border(Z_RES_PHONE_ACCENT_BRUSH);
		Zelement_set_width(av, 58);
		Zelement_set_height(av, 58);
		const Zid ic = icon_holder(symbol(Z_SYMBOL_SEND), 28, Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
		Zelement_set_horizontal_alignment(ic, Z_HALIGN_CENTER);
		Zelement_set_vertical_alignment(ic, Z_VALIGN_CENTER);
		Zborder_set_child(av, ic);
		Zgrid_place(g, av, 0, 0);
		const Zid t = text("message +" + digits, Z_RES_TEXT_STYLE_EXTRA_LARGE_FONT_SIZE, Z_FONT_SEMILIGHT, Z_RES_PHONE_FOREGROUND_BRUSH);
		set_trim(t);
		Zelement_set_margin(t, Zthickness(G, 0, 0, 0));
		Zelement_set_vertical_alignment(t, Z_VALIGN_CENTER);
		Zgrid_place(g, t, 1, 0);
		add_row(nc.list, g, digits + "@s.whatsapp.net");
		nc.keys.push_back("+" + digits);
	}
	std::string letter;
	int n = 0;
	for (const Contact* c : shown) {
		if (n++ >= 400) break;
		const std::string l = first_letter(c->name);
		if (l != letter) {
			letter = l;
			Zlist_view_add_group_header(nc.list, lower(l).c_str());
		}
		Row r;
		const Zid g = row_grid(c->jid, c->name, false, r);
		Ztext_block_set_text(r.preview, c->phone.c_str());
		add_row(nc.list, g, c->jid);
		nc.keys.push_back(c->name);
	}
	Zelement_set_visibility(nc.progress, st.b_contacts_loaded ? Z_COLLAPSED : Z_VISIBLE);
	Zelement_set_visibility(nc.empty, st.b_contacts_loaded && Zitems_control_get_count(nc.list) == 0 ? Z_VISIBLE : Z_COLLAPSED);
}
}  // namespace

void new_chat_contacts_changed() { new_chat_fill(); }

Zid page_new_chat(const Zid, void*) {
	nc = NewChat();
	nc.page = Zpage_create();
	Zid body;
	page_frame(nc.page, "WHATSAPP", "new chat", &body);
	Zgrid_add_row(body, Z_GRID_AUTO, 0);
	Zgrid_add_row(body, Z_GRID_STAR, 1);
	nc.box = Ztext_box_create(nullptr);
	Ztext_box_set_placeholder_text(nc.box, "name or number");
	Zelement_set_margin(nc.box, Zthickness(0, 0, 0, G));
	Zgrid_place(body, nc.box, 0, 0);
	ListBox lb = list_box(st.contacts.empty() && st.b_contacts_loaded ? "no contacts yet - they arrive from your phone" : "nothing found", "");
	nc.list = lb.list;
	nc.empty = lb.empty;
	nc.progress = lb.progress;
	Zgrid_place(body, lb.cell, 0, 1);
	on_text(nc.box, [] { new_chat_fill(); });
	Zlist_view_on_item_click(nc.list, [](const Zid list, const int index, void*) {
		const std::string jid = key_at(list, index);
		if (index >= 0 && size_t(index) < nc.keys.size()) open_chat(jid, nc.keys[size_t(index)]);
	}, nullptr);
	new_chat_fill();
	load_contacts();
	return nc.page;
}

/* ======== S10 new status ======== */
Zid page_new_status(const Zid, void*) {
	const Zid page = Zpage_create();
	Zid body;
	page_frame(page, "WHATSAPP", "new status", &body);
	const Zid s = Zstack_panel_create(Z_VERTICAL);
	Zpanel_add(body, s);
	const Zid box = Ztext_box_create(nullptr);
	Ztext_box_set_placeholder_text(box, "type a status");
	Ztext_box_set_accepts_return(box, true);
	Ztext_box_set_text_wrapping(box, Z_WRAP);
	Zpanel_add(s, box);
	const Zid count = text("0/700", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
	Zelement_set_horizontal_alignment(count, Z_HALIGN_RIGHT);
	Zpanel_add(s, count);
	const Zid err = text_px("", 16, Z_FONT_NORMAL, Z_RES_NONE);
	Ztext_block_set_foreground(err, literal(0xFFE51400)); /* L1 */
	Ztext_block_set_text_wrapping(err, Z_WRAP);
	Zpanel_add(s, err);
	const Zid bar = app_bar();
	Zpage_set_bottom_app_bar(page, bar);
	const Zid post_b = bar_button(bar, "post", symbol(Z_SYMBOL_SEND), [box, err] {
		const std::string t = Ztext_box_get_text(box);
		if (t.empty() || t.size() > 700) return;
		request("post_status", {{"text", t}}, [err](const json*, const char* e) {
			if (e) {
				Ztext_block_set_text(err, "not posted, your text is kept");
				return;
			}
			load_statuses();
			go_back();
		});
	});
	Zelement_set_enabled(post_b, false);
	on_text(box, [box, count, post_b] {
		const size_t n = strlen(Ztext_box_get_text(box));
		Ztext_block_set_text(count, (std::to_string(n) + "/700").c_str());
		Zelement_set_enabled(post_b, n > 0 && n <= 700 && engine_up());
	});
	return page;
}

/* ======== S11 search ======== */
namespace {
struct Search {
	Zid page = Z_INVALID_ID, box, list, empty, progress;
	std::vector<std::string> keys;
	int timer = 0;
	std::string query;
} se;

Zid snippet_row(const std::string& title, const std::string& body, const std::string& match, const long ts) {
	const Zid s = Zstack_panel_create(Z_VERTICAL);
	Zelement_set_margin(s, Zthickness(0, H, 0, H));
	const Zid top = Zgrid_create();
	Zgrid_add_column(top, Z_GRID_STAR, 1);
	Zgrid_add_column(top, Z_GRID_AUTO, 0);
	const Zid t = text(title, Z_RES_CONTENT_CONTROL_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_FOREGROUND_BRUSH);
	set_trim(t);
	Zgrid_place(top, t, 0, 0);
	if (ts) {
		const Zid tm = text(time_short(ts), Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
		Zelement_set_margin(tm, Zthickness(G, 0, 0, 0));
		Zgrid_place(top, tm, 1, 0);
	}
	Zpanel_add(s, top);
	/* the match in accent, the text after it subtle (a TextBlock has no runs) */
	const Zid line = Zstack_panel_create(Z_HORIZONTAL);
	const std::string lb = lower(body);
	const size_t at = match.empty() ? std::string::npos : lb.find(lower(match));
	std::string rest = body;
	if (at != std::string::npos) {
		const Zid mt = text(body.substr(at, match.size()), Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_ACCENT_BRUSH);
		if (at > 0) {
			const Zid pre = text("..." , Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
			Zpanel_add(line, pre);
		}
		Zpanel_add(line, mt);
		rest = body.substr(at + match.size());
	}
	const Zid r = text(one_line(rest), Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
	set_trim(r);
	Zpanel_add(line, r);
	Zelement_set_clip_to_bounds(line, true);
	Zpanel_add(s, line);
	return s;
}

void search_fill(const json* found) {
	if (se.page == Z_INVALID_ID || !Zelement_is_valid(se.page)) return;
	Zitems_control_clear(se.list);
	se.keys.clear();
	const std::string q = lower(se.query);
	if (q.empty()) {
		Zelement_set_visibility(se.empty, Z_COLLAPSED);
		return;
	}
	bool b_head = false;
	for (const Chat* c : chats_sorted(false)) {
		if (lower(c->name).find(q) == std::string::npos) continue;
		if (!b_head) {
			Zlist_view_add_group_header(se.list, "chats");
			b_head = true;
		}
		Row r;
		const Zid g = row_grid(c->id, c->name, c->b_group, r);
		Ztext_block_set_text(r.preview, one_line(c->last_text).c_str());
		add_row(se.list, g, c->id);
		se.keys.push_back(c->name);
	}
	if (found && found->contains("messages")) {
		b_head = false;
		for (auto& o : (*found)["messages"]) {
			if (!b_head) {
				Zlist_view_add_group_header(se.list, "messages");
				b_head = true;
			}
			const std::string chat = jstr(o, "chat");
			auto it = st.chats.find(chat);
			const std::string name = it != st.chats.end() ? it->second.name : jstr(o, "sender_name");
			add_row(se.list, snippet_row(name, jstr(o, "text"), se.query, jint(o, "timestamp")), chat);
			se.keys.push_back(name);
		}
	}
	Zelement_set_visibility(se.empty, Zitems_control_get_count(se.list) == 0 && found ? Z_VISIBLE : Z_COLLAPSED);
	list_entrance(se.list);
}
}  // namespace

Zid page_search(const Zid, void*) {
	se = Search();
	se.page = Zpage_create();
	Zid body;
	page_frame(se.page, "WHATSAPP", "search", &body);
	Zgrid_add_row(body, Z_GRID_AUTO, 0);
	Zgrid_add_row(body, Z_GRID_STAR, 1);
	se.box = Ztext_box_create(nullptr);
	Ztext_box_set_placeholder_text(se.box, "search chats and messages");
	Zelement_set_margin(se.box, Zthickness(0, 0, 0, G));
	Zgrid_place(body, se.box, 0, 0);
	ListBox lb = list_box("nothing found", "");
	se.list = lb.list;
	se.empty = lb.empty;
	se.progress = lb.progress;
	Zelement_set_visibility(se.progress, Z_COLLAPSED);
	Zgrid_place(body, lb.cell, 0, 1);
	on_text(se.box, [] {
		se.query = Ztext_box_get_text(se.box);
		search_fill(nullptr);
		cancel(se.timer);
		if (se.query.empty()) return;
		se.timer = after(400, [] {
			se.timer = 0;
			if (se.page == Z_INVALID_ID || !Zelement_is_valid(se.page)) return;
			Zelement_set_visibility(se.progress, Z_VISIBLE);
			const std::string q = se.query;
			request("search", {{"query", q}, {"limit", 40}}, [q](const json* r, const char* err) {
				if (se.page == Z_INVALID_ID || !Zelement_is_valid(se.page) || q != se.query) return;
				Zelement_set_visibility(se.progress, Z_COLLAPSED);
				static const json none = json::object();
				search_fill(err ? &none : r);
			});
		});
	});
	Zlist_view_on_item_click(se.list, [](const Zid list, const int index, void*) {
		if (index >= 0 && size_t(index) < se.keys.size()) open_chat(key_at(list, index), se.keys[size_t(index)]);
	}, nullptr);
	return se.page;
}

/* ======== S12 starred, archived ======== */
Zid page_starred(const Zid, void*) {
	const Zid page = Zpage_create();
	Zid body;
	page_frame(page, "WHATSAPP", "starred", &body);
	ListBox lb = list_box("no starred messages", "");
	Zpanel_add(body, lb.cell);
	const Zid list = lb.list, empty = lb.empty, progress = lb.progress;
	Zlist_view_on_item_click(list, [](const Zid l, const int index, void*) { open_chat(key_at(l, index)); }, nullptr);
	request("starred", {{"limit", 200}}, [page, list, empty, progress](const json* r, const char* err) {
		if (!Zelement_is_valid(page)) return;
		Zelement_set_visibility(progress, Z_COLLAPSED);
		int n = 0;
		if (!err && r->contains("messages"))
			for (auto& o : (*r)["messages"]) {
				const std::string chat = jstr(o, "chat");
				auto it = st.chats.find(chat);
				add_row(list, snippet_row(it != st.chats.end() ? it->second.name : jstr(o, "sender_name"), jstr(o, "text"), "", jint(o, "timestamp")), chat);
				n++;
			}
		Zelement_set_visibility(empty, n ? Z_COLLAPSED : Z_VISIBLE);
		list_entrance(list);
	});
	return page;
}

Zid page_archived(const Zid, void*) {
	const Zid page = Zpage_create();
	Zid body;
	page_frame(page, "WHATSAPP", "archived", &body);
	ListBox lb = list_box("no archived chats", "");
	Zpanel_add(body, lb.cell);
	Zelement_set_visibility(lb.progress, Z_COLLAPSED);
	int n = 0;
	for (const Chat* c : chats_sorted(true)) {
		Row r;
		const Zid g = row_grid(c->id, c->name, c->b_group, r);
		chat_row_fill(*c, r);
		add_row(lb.list, g, c->id);
		n++;
	}
	Zelement_set_visibility(lb.empty, n ? Z_COLLAPSED : Z_VISIBLE);
	Zlist_view_on_item_click(lb.list, [](const Zid l, const int index, void*) { open_chat(key_at(l, index)); }, nullptr);
	return page;
}

/* ======== S13 follow channel ======== */
Zid page_follow(const Zid, void*) {
	const Zid page = Zpage_create();
	Zid body;
	page_frame(page, "WHATSAPP", "follow channel", &body);
	const Zid s = Zstack_panel_create(Z_VERTICAL);
	Zpanel_add(body, s);
	const Zid box = Ztext_box_create(nullptr);
	Ztext_box_set_header(box, "invite link or code");
	Zpanel_add(s, box);
	const Zid err = text_px("", 16, Z_FONT_NORMAL, Z_RES_NONE);
	Ztext_block_set_foreground(err, literal(0xFFE51400)); /* L1 */
	Ztext_block_set_text_wrapping(err, Z_WRAP);
	Zpanel_add(s, err);
	const Zid bar = app_bar();
	Zpage_set_bottom_app_bar(page, bar);
	bar_button(bar, "follow", symbol(Z_SYMBOL_ACCEPT), [box, err] {
		const std::string code = Ztext_box_get_text(box);
		if (code.empty()) return;
		request("channel_follow", {{"code", code}}, [err](const json*, const char* e) {
			if (e) {
				if (Zelement_is_valid(err)) Ztext_block_set_text(err, "could not follow that channel");
				return;
			}
			load_channels();
			go_back();
		});
	});
	return page;
}

}  // namespace wa
