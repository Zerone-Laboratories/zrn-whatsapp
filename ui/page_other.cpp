// SPDX-License-Identifier: GPL-2.0-or-later
/* S4 chat info, S5 settings, S6 account, S7 link, S8 media viewer, S9 status viewer (spec 2.5-2.9). */
#include "wa.h"

#include <cmath>
#include <cstring>

namespace wa {
namespace {

Zid group_header(const char* t) {
	const Zid h = Ztext_block_create(t);
	Ztext_block_set_style(h, Z_TEXT_STYLE_GROUP_HEADER);
	Ztext_block_bind_foreground(h, Z_RES_PHONE_ACCENT_BRUSH);
	Zelement_set_margin(h, Zthickness(0, M, 0, G));
	return h;
}

Zid toggle(const char* header, const bool b_on, std::function<void(bool)> fn, const float bottom = M) {
	const Zid t = Ztoggle_switch_create();
	Ztoggle_switch_set_header(t, header);
	Ztoggle_switch_set_is_on(t, b_on);
	Zelement_set_margin(t, Zthickness(0, 0, 0, bottom));
	auto* f = new std::function<void(bool)>(std::move(fn)); /* lives as long as the switch; a few per page */
	Ztoggle_switch_on_toggled(t, [](const Zid s, void* ud) { (*static_cast<std::function<void(bool)>*>(ud))(Ztoggle_switch_get_is_on(s)); }, f);
	return t;
}

Zid scroll_stack(const Zid body, Zid* stack) {
	const Zid sv = Zscroll_viewer_create();
	*stack = Zstack_panel_create(Z_VERTICAL);
	Zscroll_viewer_set_content(sv, *stack);
	Zpanel_add(body, sv);
	return sv;
}

Zid error_line() {
	const Zid e = text_px("", 16, Z_FONT_NORMAL, Z_RES_NONE);
	Ztext_block_set_foreground(e, literal(0xFFE51400)); /* L1 */
	Ztext_block_set_text_wrapping(e, Z_WRAP);
	return e;
}

}  // namespace

/* ======== S4 ======== */
Zid page_info(const Zid, void* const p) {
	const std::string id = p ? *static_cast<const std::string*>(p) : open_chat_id();
	auto it = st.chats.find(id);
	const std::string name = it != st.chats.end() ? it->second.name : id.substr(0, id.find('@'));
	const bool b_group = it != st.chats.end() ? it->second.b_group : id.find("@g.us") != std::string::npos;
	const Zid page = Zpage_create();
	Zid body;
	/* the engine lists no media of a chat: no pivot, a plain header (spec 2.5) */
	page_frame(page, upper(name).c_str(), "info", &body);
	Zid s;
	scroll_stack(body, &s);
	const Zid pic = Zimage_create();
	Zelement_set_width(pic, 322);
	Zelement_set_height(pic, 322);
	Zimage_set_stretch(pic, Z_STRETCH_UNIFORM_TO_FILL);
	const Zid box = border(Z_RES_PHONE_ACCENT_BRUSH);
	Zelement_set_width(box, 322);
	Zelement_set_height(box, 322);
	Zelement_set_horizontal_alignment(box, Z_HALIGN_LEFT);
	Zborder_set_child(box, pic);
	Zelement_set_visibility(pic, Z_COLLAPSED);
	const Zid tap = tappable(box);
	Zelement_set_horizontal_alignment(tap, Z_HALIGN_LEFT);
	Zpanel_add(s, tap);
	auto path = std::make_shared<std::string>();
	request("profile_picture", {{"jid", id}, {"preview", false}}, [pic, path](const json* r, const char* err) {
		if (err) return;
		*path = jstr(*r, "path");
		image_file(*path, 322, 322, true, [pic](PixP px) { image_set(pic, px); });
	});
	click(tap, [path, id] {
		if (path->empty()) return;
		g_media_view = {id, "", *path, "", nullptr};
		nav(page_media, "", gap::slide_up());
	});
	if (!b_group) {
		Zpanel_add(s, group_header("about and phone number"));
		const size_t at = id.find("@s.whatsapp.net");
		const Zid num = text(at != std::string::npos ? "+" + id.substr(0, at) : "hidden by privacy", Z_RES_CONTENT_CONTROL_FONT_SIZE,
			Z_FONT_NORMAL, Z_RES_PHONE_FOREGROUND_BRUSH);
		Zpanel_add(s, num);
		Zpanel_add(s, text("mobile", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR));
	}
	Zpanel_add(s, group_header("settings"));
	const bool b_muted = it != st.chats.end() && it->second.b_muted;
	const bool b_pinned = it != st.chats.end() && it->second.b_pinned;
	const bool b_arch = it != st.chats.end() && it->second.b_archived;
	/* mute and archive follow the phone (IPC.md: read-only); pin is this phone's */
	const Zid mute = toggle("mute", b_muted, [](bool) {}, G);
	Zelement_set_enabled(mute, false);
	Zpanel_add(s, mute);
	Zpanel_add(s, toggle("pin to top", b_pinned, [id](const bool on) {
		request("pin_chat", {{"chat", id}, {"pinned", on}}, [](const json*, const char* err) { if (!err) load_chats(); });
	}, G));
	const Zid arch = toggle("archive", b_arch, [](bool) {}, G);
	Zelement_set_enabled(arch, false);
	Zpanel_add(s, arch);
	Zpanel_add(s, text("Mute and archive are changed on your phone.", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR));
	if (b_group) {
		const Zid ph = group_header("participants");
		Zpanel_add(s, ph);
		const Zid people = Zstack_panel_create(Z_VERTICAL);
		Zpanel_add(s, people);
		request("group_info", {{"chat", id}}, [people, ph](const json* r, const char* err) {
			if (err || !Zelement_is_valid(people) || !r->contains("participants")) return;
			auto& ps = (*r)["participants"];
			Ztext_block_set_text(ph, (std::to_string(ps.size()) + " participants").c_str());
			int n = 0;
			for (auto& m : ps) {
				if (n++ >= 256) break;
				const Zid g = Zgrid_create();
				Zgrid_add_column(g, Z_GRID_PIXEL, 58);
				Zgrid_add_column(g, Z_GRID_STAR, 1);
				Zgrid_add_column(g, Z_GRID_AUTO, 0);
				Zelement_set_margin(g, Zthickness(0, G, 0, G));
				const std::string nm = jstr(m, "name").empty() ? jstr(m, "jid").substr(0, jstr(m, "jid").find('@')) : jstr(m, "name");
				Zgrid_place(g, avatar(jstr(m, "jid"), nm, 58, false), 0, 0);
				const Zid t = text(nm, Z_RES_TEXT_STYLE_EXTRA_LARGE_FONT_SIZE, Z_FONT_SEMILIGHT, Z_RES_PHONE_FOREGROUND_BRUSH);
				set_trim(t);
				Zelement_set_margin(t, Zthickness(G, 0, 0, 0));
				Zelement_set_vertical_alignment(t, Z_VALIGN_CENTER);
				Zgrid_place(g, t, 1, 0);
				if (jbool(m, "is_admin") || jbool(m, "is_super_admin")) {
					const Zid a = text("admin", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_ACCENT_BRUSH);
					Zelement_set_vertical_alignment(a, Z_VALIGN_CENTER);
					Zelement_set_margin(a, Zthickness(G, 0, 0, 0));
					Zgrid_place(g, a, 2, 0);
				}
				Zpanel_add(people, g);
			}
		});
	}
	Zpanel_add(s, group_header("encryption"));
	const Zid enc = text("Messages are end-to-end encrypted. This phone is a linked device of your account.", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE,
		Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
	Ztext_block_set_text_wrapping(enc, Z_WRAP);
	Zelement_set_margin(enc, Zthickness(0, 0, 0, M));
	Zpanel_add(s, enc);
	return page;
}

/* ======== S5 ======== */
Zid page_settings(const Zid, void*) {
	const Zid page = Zpage_create();
	Zid body;
	page_frame(page, "WHATSAPP", "settings", &body);
	Zid s;
	scroll_stack(body, &s);
	Zpanel_add(s, toggle("notifications", st.prefs.b_notify, [](const bool on) { st.prefs.b_notify = on; prefs_save(); }));
	Zpanel_add(s, toggle("show message preview", st.prefs.b_preview, [](const bool on) { st.prefs.b_preview = on; prefs_save(); }));
	Zpanel_add(s, toggle("enter key sends", st.prefs.b_enter_sends, [](const bool on) { st.prefs.b_enter_sends = on; prefs_save(); }));
	Zpanel_add(s, toggle("download stickers automatically", st.prefs.b_stickers, [](const bool on) { st.prefs.b_stickers = on; prefs_save(); }));
	Zpanel_add(s, toggle("use WhatsApp green instead of the accent", st.prefs.b_green, [](const bool on) {
		st.prefs.b_green = on;
		prefs_save();
		theme_apply(); /* live: the root's accent, every bound element follows */
	}));
	const Zid acc = Zstack_panel_create(Z_VERTICAL);
	Zpanel_add(acc, text("account", Z_RES_TEXT_STYLE_EXTRA_LARGE_FONT_SIZE, Z_FONT_SEMILIGHT, Z_RES_PHONE_FOREGROUND_BRUSH));
	Zpanel_add(acc, text("linked device, unlink", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR));
	const Zid tap = tappable(acc);
	Zelement_set_margin(tap, Zthickness(0, 0, 0, M));
	click(tap, [] { nav(page_account); });
	Zpanel_add(s, tap);
	const Zid about = text("WhatsApp for ztOS 0.3.0. An unofficial client: it is not made or approved by WhatsApp.",
		Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
	Ztext_block_set_text_wrapping(about, Z_WRAP);
	Zelement_set_margin(about, Zthickness(0, 0, 0, M));
	Zpanel_add(s, about);
	return page;
}

/* ======== S6 ======== */
Zid page_account(const Zid, void*) {
	const Zid page = Zpage_create();
	Zid body;
	page_frame(page, "WHATSAPP", "account", &body);
	Zid s;
	scroll_stack(body, &s);
	const std::string self = st.self_jid.empty() ? "" : "+" + st.self_jid.substr(0, st.self_jid.find_first_of(":@"));
	Zpanel_add(s, text(self.empty() ? "your number" : self, Z_RES_TEXT_STYLE_EXTRA_LARGE_FONT_SIZE, Z_FONT_SEMILIGHT, Z_RES_PHONE_FOREGROUND_BRUSH));
	const Zid state = text("linked device, " + st.connection, Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
	Zelement_set_margin(state, Zthickness(0, 0, 0, M));
	Zpanel_add(s, state);
	const Zid b = Zbutton_create("unlink this phone");
	Zelement_set_horizontal_alignment(b, Z_HALIGN_STRETCH);
	Zpanel_add(s, b);
	const Zid err = error_line();
	Zpanel_add(s, err);
	click(b, [err] {
		confirm("unlink?", "Messages stored on this phone are deleted.", "unlink", [err] {
			request("logout", json::object(), [err](const json*, const char* e) {
				if (e && Zelement_is_valid(err)) Ztext_block_set_text(err, "not unlinked: the WhatsApp service did not answer");
			});
		});
	});
	return page;
}

/* ======== S7 ======== */
namespace {
Zid g_link_page = Z_INVALID_ID, g_link_body = Z_INVALID_ID;
std::string g_link_shown;
std::string g_phone;

void link_build() {
	if (!Zelement_is_valid(g_link_body)) return;
	std::string state = !engine_up() ? "no_engine" : st.pair_state.empty() || st.pair_state == "paired" ? "no_engine" : st.pair_state;
	if (engine_up() && st.pair_state.empty()) state = "waiting";
	const std::string key = state + "|" + st.pair_qr + "|" + st.pair_code + "|" + st.pair_error + (st.b_logged_out ? "|out" : "");
	if (key == g_link_shown) return;
	g_link_shown = key;
	Zpanel_clear(g_link_body);
	Zid s;
	scroll_stack(g_link_body, &s);
	auto para = [&](const std::string& t, const bool b_subtle = false) {
		const Zid p = text(t, Z_RES_TEXT_STYLE_LARGE_FONT_SIZE, Z_FONT_NORMAL, b_subtle ? Z_RES_PHONE_BASE_MEDIUM_MID_COLOR : Z_RES_PHONE_FOREGROUND_BRUSH);
		Ztext_block_set_text_wrapping(p, Z_WRAP);
		Zelement_set_margin(p, Zthickness(0, 0, 0, M));
		Zpanel_add(s, p);
		return p;
	};
	auto button = [&](const char* label, std::function<void()> fn) {
		const Zid b = Zbutton_create(label);
		Zelement_set_horizontal_alignment(b, Z_HALIGN_STRETCH);
		Zelement_set_margin(b, Zthickness(0, 0, 0, G));
		click(b, std::move(fn));
		Zpanel_add(s, b);
		return b;
	};
	if (state == "no_engine" || state == "waiting") {
		const Zid bar = top_progress();
		Zelement_set_margin(bar, Zthickness(-M, -22.5f, -M, M));
		Zpanel_add(s, bar);
		para(state == "no_engine" ? "waiting for the WhatsApp service..." : "connecting...", true);
	} else if (state == "qr") {
		para("On your phone open WhatsApp, then linked devices, link a device, and scan this code.");
		int modules = 0;
		PixP q = qr_pix(st.pair_qr, &modules);
		if (q) {
			/* a whole number of device pixels a module, at most 304 px */
			const float scale = ui_scale();
			const float each = std::floor(304.0f * scale / float(modules));
			const float side = each * float(modules) / scale;
			const Zid img = Zimage_create();
			Zimage_set_stretch(img, Z_STRETCH_FILL);
			Zelement_set_width(img, side);
			Zelement_set_height(img, side);
			Zelement_set_horizontal_alignment(img, Z_HALIGN_CENTER);
			image_set(img, q);
			Zpanel_add(s, img);
		}
		const Zid bar = top_progress();
		Zelement_set_margin(bar, Zthickness(0, G, 0, M));
		Zpanel_add(s, bar);
		button("cancel", [] { request("pair_cancel"); });
	} else if (state == "phone_code") {
		para("On your phone open WhatsApp, then linked devices, link a device, link with phone number instead, and type this code.");
		const Zid code = Ztext_box_create(st.pair_code.c_str());
		Ztext_box_set_is_read_only(code, true);
		Zcontrol_set_border_thickness(code, Zthickness_uniform(0));
		Zcontrol_set_background(code, Zcolor_rgba(0, 0, 0, 0));
		Zcontrol_bind_font_size(code, Z_RES_TEXT_STYLE_EXTRA_EXTRA_LARGE_FONT_SIZE);
		Zcontrol_set_font_family(code, Z_FONT_SEMIBOLD);
		Zcontrol_bind_foreground(code, Z_RES_PHONE_FOREGROUND_BRUSH);
		Zpanel_add(s, code);
		button("cancel", [] { request("pair_cancel"); });
	} else if (state == "pairing") {
		const Zid ring = Zprogress_ring_create();
		Zprogress_ring_set_is_active(ring, true);
		Zelement_set_width(ring, 46);
		Zelement_set_height(ring, 46);
		Zelement_set_horizontal_alignment(ring, Z_HALIGN_CENTER);
		Zelement_set_margin(ring, Zthickness(0, M, 0, M));
		Zpanel_add(s, ring);
		const Zid t = para("finishing the link...");
		Ztext_block_set_text_alignment(t, Z_TEXT_CENTER);
	} else {
		para("This is an unofficial WhatsApp client. WhatsApp's terms do not allow it, and a number can be banned for using one. "
			 "Link it only if you accept that.");
		const Zid err = error_line();
		Zelement_set_margin(err, Zthickness(0, 0, 0, G));
		if (st.b_logged_out) Ztext_block_set_text(err, "this device was unlinked from your phone");
		else if (!st.pair_error.empty()) Ztext_block_set_text(err, "linking failed, try again");
		Zpanel_add(s, err);
		button("show QR code", [] { request("pair_qr"); });
		const Zid phone = Ztext_box_create(g_phone.c_str());
		Ztext_box_set_header(phone, "phone number with country code");
		on_text(phone, [phone] { g_phone = Ztext_box_get_text(phone); });
		Zpanel_add(s, phone);
		const Zid perr = error_line();
		button("get a code", [perr] {
			std::string d;
			for (const char c : g_phone)
				if (isdigit(static_cast<unsigned char>(c))) d += c;
			if (d.size() < 8) {
				Ztext_block_set_text(perr, "type your whole number, with its country code");
				return;
			}
			request("pair_phone", {{"phone", d}}, [perr](const json*, const char* e) {
				if (e && Zelement_is_valid(perr)) Ztext_block_set_text(perr, "that did not work, check the number");
			});
		});
		Zpanel_add(s, perr);
	}
}
}  // namespace

void link_changed() { link_build(); }

Zid page_link(const Zid, void*) {
	g_link_page = Zpage_create();
	page_frame(g_link_page, "WHATSAPP", "link device", &g_link_body);
	g_link_shown.clear();
	link_build();
	return g_link_page;
}

/* ======== S8 media viewer (spec 2.8): black, chrome toggles on tap, double tap 1x / 2.5x ======== */
namespace {
void slide_in(const Zid content) {
	if (gap::have_slide_up()) return;
	Zelement_set_translate(content, 0, 150);
	Zelement_set_opacity(content, 0);
	Zelement_animate(content, Z_PROP_TRANSLATE_Y, 0, 350, Z_EASE_EMPHASIZED, 0);
	Zelement_animate(content, Z_PROP_OPACITY, 1, 350, Z_EASE_EMPHASIZED, 0);
}
}  // namespace

Zid page_media(const Zid, void*) {
	const MediaView v = g_media_view;
	const Zid page = Zpage_create();
	const Zid g = Zgrid_create();
	Zpanel_set_background(g, literal(0xFF000000)); /* L2 */
	Zpage_set_content(page, g);
	const Zid img = Zimage_create();
	Zimage_set_stretch(img, Z_STRETCH_UNIFORM);
	if (v.thumb) image_set(img, v.thumb);
	const Zid tap = tappable(img);
	Zpanel_add(g, tap);
	const Zid chrome = Zstack_panel_create(Z_VERTICAL);
	Zelement_set_vertical_alignment(chrome, Z_VALIGN_TOP);
	Zelement_set_margin(chrome, Zthickness(M, G, M, 0));
	const Zid cap = text_px(v.caption, 16, Z_FONT_NORMAL, Z_RES_NONE);
	Ztext_block_set_foreground(cap, literal(0xFFFFFFFF));
	Ztext_block_set_text_wrapping(cap, Z_WRAP);
	Zpanel_add(chrome, cap);
	Zpanel_add(g, chrome);
	const Zid err = text_px("", 16, Z_FONT_NORMAL, Z_RES_NONE);
	Ztext_block_set_foreground(err, literal(0xFFFFFFFF));
	Zelement_set_horizontal_alignment(err, Z_HALIGN_CENTER);
	Zelement_set_vertical_alignment(err, Z_VALIGN_CENTER);
	Zpanel_add(g, err);
	const Zid bar = app_bar();
	Zpage_set_bottom_app_bar(page, bar);
	auto path = std::make_shared<std::string>(v.path);
	bar_button(bar, "save", symbol(Z_SYMBOL_SAVE), [path, v] {
		if (path->empty()) return;
		const char* home = getenv("HOME");
		const std::string to = std::string(home ? home : "") + "/Pictures/whatsapp-" + (v.id.empty() ? "picture" : v.id) + ".jpg";
		const std::string from = *path;
		work([from, to] {
			FILE* a = fopen(from.c_str(), "rb");
			FILE* b = a ? fopen(to.c_str(), "wb") : nullptr;
			char buf[65536];
			size_t n;
			while (a && b && (n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
			if (a) fclose(a);
			if (b) fclose(b);
		});
	});
	const Zid share = bar_button(bar, "share", symbol(Z_SYMBOL_UPLOAD), [] {});
	Zelement_set_enabled(share, false);
	Zelement_set_tag(share, reinterpret_cast<void*>(1));
	auto show = [img](const std::string& p) { image_file(p, 360, 740, false, [img](PixP px) { image_set(img, px); }); };
	if (!v.path.empty()) show(v.path);
	else if (!v.id.empty())
		request("download_media", {{"chat", v.chat}, {"message_id", v.id}}, [show, path, err, chat = v.chat, id = v.id](const json* r, const char* e) {
			const std::string p = e ? "" : jstr(*r, "path");
			if (p.empty()) {
				if (Zelement_is_valid(err)) Ztext_block_set_text(err, "could not download this picture");
				return;
			}
			*path = p;
			show(p);
			chat_media_downloaded(chat, id, p);
		});
	auto state = std::make_shared<std::pair<long, bool>>(0, false); /* last tap ms, zoomed */
	click(tap, [img, chrome, bar, state] {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		const long now = long(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
		if (now - state->first < 300) {
			state->second = !state->second;
			Zelement_animate(img, Z_PROP_SCALE, state->second ? 2.5f : 1.0f, 250, Z_EASE_OUT, 0);
			state->first = 0;
			return;
		}
		state->first = now;
		const bool b_show = Zelement_get_opacity(chrome) < 0.5f;
		Zelement_animate(chrome, Z_PROP_OPACITY, b_show ? 1 : 0, 167, Z_EASE_LINEAR, 0);
		Zelement_set_visibility(bar, b_show ? Z_VISIBLE : Z_COLLAPSED);
	});
	slide_in(g);
	return page;
}

/* ======== S9 status viewer (spec 2.9) ======== */
namespace {
struct StatusView {
	Zid page = Z_INVALID_ID, content, name, time;
	std::vector<Zid> bars;
	int person = -1, at = 0, timer = 0;
} sv;

void status_show();

void status_step(const int d) {
	if (!Zelement_is_valid(sv.page) || sv.person < 0 || size_t(sv.person) >= st.statuses.size()) return;
	const auto& items = st.statuses[size_t(sv.person)].items;
	const int n = sv.at + d;
	if (n < 0) return;
	if (size_t(n) >= items.size()) {
		go_back();
		return;
	}
	sv.at = n;
	status_show();
}

void status_show() {
	cancel(sv.timer);
	StatusPerson& p = st.statuses[size_t(sv.person)];
	if (size_t(sv.at) >= p.items.size()) return;
	StatusItem& it = p.items[size_t(sv.at)];
	for (size_t k = 0; k < sv.bars.size(); k++) Zrange_base_set_value(sv.bars[k], int(k) < sv.at ? 100 : 0);
	Ztext_block_set_text(sv.time, time_short(it.ts).c_str());
	Zpanel_clear(sv.content);
	if (it.kind == "image") {
		const Zid img = Zimage_create();
		Zimage_set_stretch(img, Z_STRETCH_UNIFORM);
		Zpanel_add(sv.content, img);
		if (!it.thumb.empty()) image_bytes(unbase64(it.thumb), 90, 160, [img](PixP x) { image_set(img, x); });
		auto load = [img](const std::string& path) { image_file(path, 360, 740, false, [img](PixP x) { image_set(img, x); }); };
		if (!it.path.empty()) load(it.path);
		else request("download_media", {{"chat", "status@broadcast"}, {"message_id", it.id}}, [load](const json* r, const char* e) {
			const std::string path = e ? "" : jstr(*r, "path");
			if (!path.empty()) load(path);
		});
		if (!it.text.empty()) {
			const Zid cap = text_px(it.text, 16, Z_FONT_NORMAL, Z_RES_NONE);
			Ztext_block_set_foreground(cap, literal(0xFFFFFFFF));
			Ztext_block_set_text_wrapping(cap, Z_WRAP);
			Zelement_set_vertical_alignment(cap, Z_VALIGN_BOTTOM);
			Zelement_set_margin(cap, Zthickness(M, 0, M, 57.5f));
			Zpanel_add(sv.content, cap);
		}
	} else {
		const Zid b = border(Z_RES_PHONE_ACCENT_BRUSH);
		const Zid t = text(it.kind == "video" ? "video updates can only be seen on your phone" : it.text, Z_RES_TEXT_STYLE_EXTRA_EXTRA_LARGE_FONT_SIZE,
			Z_FONT_SEMILIGHT, Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
		Ztext_block_set_text_wrapping(t, Z_WRAP);
		Ztext_block_set_text_alignment(t, Z_TEXT_CENTER);
		Zelement_set_vertical_alignment(t, Z_VALIGN_CENTER);
		Zelement_set_margin(t, Zthickness_uniform(M));
		Zborder_set_child(b, t);
		Zpanel_add(sv.content, b);
	}
	if (!it.b_seen) {
		it.b_seen = true;
		if (p.unseen > 0) p.unseen--;
		request("status_seen", {{"sender", p.sender}, {"ids", json::array({it.id})}});
	}
	/* the bar of this item fills over the 5 s */
	if (size_t(sv.at) < sv.bars.size()) {
		const Zid bar = sv.bars[size_t(sv.at)];
		for (int k = 1; k <= 10; k++)
			after(500 * k, [bar, k] { if (Zelement_is_valid(bar)) Zrange_base_set_value(bar, 10 * k); });
	}
	sv.timer = after(5000, [] { status_step(1); });
}
}  // namespace

Zid page_status_view(const Zid, void*) {
	sv = StatusView();
	sv.person = g_status_person;
	sv.page = Zpage_create();
	const Zid g = Zgrid_create();
	Zpanel_set_background(g, literal(0xFF000000)); /* L2 */
	Zpage_set_content(sv.page, g);
	sv.content = Zgrid_create();
	Zpanel_add(g, sv.content);
	if (sv.person < 0 || size_t(sv.person) >= st.statuses.size()) return sv.page;
	const StatusPerson& p = st.statuses[size_t(sv.person)];
	const Zid top = Zstack_panel_create(Z_VERTICAL);
	Zelement_set_vertical_alignment(top, Z_VALIGN_TOP);
	Zelement_set_margin(top, Zthickness_uniform(G));
	const Zid segs = Zgrid_create();
	for (size_t k = 0; k < p.items.size(); k++) {
		Zgrid_add_column(segs, Z_GRID_STAR, 1);
		const Zid b = Zprogress_bar_create();
		Zrange_base_set_range(b, 0, 100);
		Zelement_set_height(b, 2);
		Zcontrol_set_foreground(b, literal(0xFFFFFFFF));
		Zcontrol_set_background(b, literal(0x4DFFFFFF));
		Zelement_set_margin(b, Zthickness(k ? H / 2 : 0, 0, k + 1 < p.items.size() ? H / 2 : 0, 0));
		Zgrid_place(segs, b, int(k), 0);
		sv.bars.push_back(b);
	}
	Zpanel_add(top, segs);
	sv.name = text_px(p.name, 18.14f, Z_FONT_SEMIBOLD, Z_RES_NONE);
	Ztext_block_set_foreground(sv.name, literal(0xFFFFFFFF));
	Zelement_set_margin(sv.name, Zthickness(0, G, 0, 0));
	Zpanel_add(top, sv.name);
	sv.time = text_px("", 16, Z_FONT_NORMAL, Z_RES_NONE);
	Ztext_block_set_foreground(sv.time, literal(0xB3FFFFFF));
	Zpanel_add(top, sv.time);
	Zpanel_add(g, top);
	/* left half back, right half next */
	const Zid halves = Zgrid_create();
	Zgrid_add_column(halves, Z_GRID_STAR, 1);
	Zgrid_add_column(halves, Z_GRID_STAR, 1);
	for (int k = 0; k < 2; k++) {
		const Zid area = Zgrid_create();
		Zpanel_set_background(area, Zcolor_rgba(0, 0, 0, 0));
		const Zid t = tappable(area);
		Zelement_set_tilt(t, false);
		click(t, [k] { status_step(k ? 1 : -1); });
		Zgrid_place(halves, t, k, 0);
	}
	Zpanel_add(g, halves);
	sv.at = 0;
	for (size_t k = 0; k < p.items.size(); k++)
		if (!p.items[k].b_seen) {
			sv.at = int(k);
			break;
		}
	status_show();
	Zpage_on_navigated_from(sv.page, [](Zid, void*) {
		cancel(sv.timer);
		main_statuses_changed();
	}, nullptr);
	slide_in(g);
	return sv.page;
}

}  // namespace wa
