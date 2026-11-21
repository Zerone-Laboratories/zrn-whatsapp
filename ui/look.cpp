// SPDX-License-Identifier: GPL-2.0-or-later
/* The controls of the spec, each bound to theme keys so a theme or accent change repaints with no rebuild (spec 5). */
#include "wa.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace wa {
namespace {

/* ---- path data: filled polygons on a fixed box (two empty corners keep the scale of a glyph family equal) ---- */
struct PathB {
	std::string s;
	void pt(const char c, const float x, const float y) {
		char b[48];
		snprintf(b, sizeof b, "%c%.2f,%.2f ", c, double(x), double(y));
		s += b;
	}
	void poly(const std::vector<std::pair<float, float>>& p) {
		for (size_t i = 0; i < p.size(); i++) pt(i ? 'L' : 'M', p[i].first, p[i].second);
		s += "Z ";
	}
	void box(const float w, const float h) {
		pt('M', 0, 0);
		pt('L', 0, 0);
		pt('L', 0, 0);
		s += "Z ";
		pt('M', w, h);
		pt('L', w, h);
		pt('L', w, h);
		s += "Z ";
	}
	void disc(const float cx, const float cy, const float r, const int n = 24) {
		std::vector<std::pair<float, float>> p;
		for (int i = 0; i < n; i++) {
			const float a = float(i) * 6.2831853f / float(n);
			p.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
		}
		poly(p);
	}
	/* a band between two radii from angle a0 to a1 (degrees, 0 = right, clockwise on screen) */
	void band(const float cx, const float cy, const float ro, const float ri, const float a0, const float a1, const int n = 24) {
		std::vector<std::pair<float, float>> p;
		for (int i = 0; i <= n; i++) {
			const float a = (a0 + (a1 - a0) * float(i) / float(n)) * 3.14159265f / 180.0f;
			p.push_back({cx + ro * std::cos(a), cy + ro * std::sin(a)});
		}
		for (int i = n; i >= 0; i--) {
			const float a = (a0 + (a1 - a0) * float(i) / float(n)) * 3.14159265f / 180.0f;
			p.push_back({cx + ri * std::cos(a), cy + ri * std::sin(a)});
		}
		poly(p);
	}
	/* a whole ring as a keyhole: one simple polygon, so it fills the same under any fill rule */
	void ring(const float cx, const float cy, const float ro, const float ri) { band(cx, cy, ro, ri, -89.0f, 269.0f, 40); }
	void rect(const float x, const float y, const float w, const float h) { poly({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}); }
};

std::string make(void (*fn)(PathB&)) {
	PathB b;
	fn(b);
	return b.s;
}

/* Callbacks of zrnui carry a pointer: the std::functions live here, keyed by the element, swept when it is gone. */
std::unordered_map<Zid, std::function<void()>> g_handlers;
size_t g_handlers_swept = 64;

void sweep() {
	if (g_handlers.size() < g_handlers_swept) return;
	for (auto it = g_handlers.begin(); it != g_handlers.end();) {
		if (!Zelement_is_valid(it->first)) it = g_handlers.erase(it);
		else ++it;
	}
	g_handlers_swept = g_handlers.size() * 2 + 64;
}

void trampoline(const Zid sender, void* const ud) {
	(void)ud;
	auto it = g_handlers.find(sender);
	if (it == g_handlers.end()) return;
	auto fn = it->second;  /* the handler may destroy the sender */
	fn();
}

std::unordered_map<Zid, std::function<void()>> g_text_handlers;
void text_trampoline(const Zid sender, void* const ud) {
	(void)ud;
	auto it = g_text_handlers.find(sender);
	if (it == g_text_handlers.end()) return;
	auto fn = it->second;
	fn();
}

std::vector<Zid> g_banners;
bool g_banner_on;
int g_banner_timer;
std::string g_banner_was;

}  // namespace

namespace glyph {
const std::string tick_one = make([](PathB& b) {
	b.box(16, 11);
	b.poly({{1.5f, 5.8f}, {2.9f, 4.4f}, {5.6f, 7.1f}, {12.1f, 0.6f}, {13.5f, 2.0f}, {5.6f, 9.9f}});
});
const std::string tick_two = make([](PathB& b) {
	b.box(16, 11);
	b.poly({{0.2f, 5.8f}, {1.6f, 4.4f}, {4.3f, 7.1f}, {10.8f, 0.6f}, {12.2f, 2.0f}, {4.3f, 9.9f}});
	b.poly({{6.6f, 8.6f}, {13.6f, 1.6f}, {15.0f, 3.0f}, {6.6f, 11.0f}, {5.9f, 10.3f}});
});
const std::string tick_clock = make([](PathB& b) {
	b.box(16, 11);
	b.ring(8, 5.5f, 5.3f, 4.0f);
	b.rect(7.4f, 2.4f, 1.2f, 3.6f);
	b.rect(7.4f, 4.9f, 2.8f, 1.1f);
});
const std::string chevron_down = make([](PathB& b) {
	b.box(16, 16);
	b.poly({{2, 5.5f}, {3.6f, 3.9f}, {8, 8.3f}, {12.4f, 3.9f}, {14, 5.5f}, {8, 11.5f}});
});
const std::string people = make([](PathB& b) {
	b.box(28, 28);
	b.disc(10, 9, 4.5f);
	b.band(10, 24, 9, 0.01f, 180, 360, 20);
	b.disc(19.5f, 10, 3.6f);
	b.band(19.5f, 23.5f, 7.2f, 0.01f, 180, 360, 20);
});
const std::string mute = make([](PathB& b) {
	b.box(16, 16);
	b.poly({{1, 5.5f}, {4, 5.5f}, {8, 2}, {8, 14}, {4, 10.5f}, {1, 10.5f}});
	b.poly({{9.5f, 5.3f}, {10.3f, 4.5f}, {15.5f, 9.7f}, {14.7f, 10.5f}});
	b.poly({{9.5f, 9.7f}, {14.7f, 4.5f}, {15.5f, 5.3f}, {10.3f, 10.5f}});
});
const std::string pin = make([](PathB& b) {
	b.box(16, 16);
	b.poly({{9.5f, 1}, {15, 6.5f}, {13.6f, 7.2f}, {10.6f, 10.2f}, {10.6f, 12.4f}, {9.7f, 13.3f}, {2.7f, 6.3f}, {3.6f, 5.4f},
		{5.8f, 5.4f}, {8.8f, 2.4f}});
	b.poly({{5.2f, 10.0f}, {6.0f, 10.8f}, {1.6f, 15.2f}, {0.8f, 14.4f}});
});
const std::string emoji = make([](PathB& b) {
	b.box(20, 20);
	b.ring(10, 10, 9, 7.6f);
	b.disc(7, 7.8f, 1.2f, 12);
	b.disc(13, 7.8f, 1.2f, 12);
	b.band(10, 10.5f, 5, 3.7f, 20, 160, 16);
});
const std::string attach = make([](PathB& b) {
	b.box(20, 20);
	b.band(10, 5.5f, 4.5f, 3.3f, 180, 360, 12);
	b.rect(5.5f, 5.5f, 1.2f, 9);
	b.rect(13.3f, 5.5f, 1.2f, 7);
	b.band(9.0f, 14.5f, 3.5f, 2.3f, 0, 180, 12);
	b.band(10, 12.5f, 2.2f, 1.0f, 0, 180, 8);
	b.rect(8, 6.5f, 1.0f, 6);
	b.rect(11.2f, 6.5f, 1.0f, 6);
	b.band(10, 6.5f, 2.2f, 1.0f, 180, 360, 8);
});
const std::string mic = make([](PathB& b) {
	b.box(20, 20);
	b.band(10, 5, 3, 0.01f, 180, 360, 12);
	b.rect(7, 5, 6, 5);
	b.band(10, 10, 3, 0.01f, 0, 180, 12);
	b.band(10, 10, 5.5f, 4.3f, 0, 180, 16);
	b.rect(9.4f, 15, 1.2f, 3);
	b.rect(6.5f, 17.8f, 7, 1.2f);
});
const std::string page = make([](PathB& b) {
	b.box(20, 20);
	b.poly({{3, 1}, {12, 1}, {17, 6}, {17, 19}, {3, 19}, {3, 17.8f}, {15.8f, 17.8f}, {15.8f, 6.5f}, {11.5f, 2.2f}, {4.2f, 2.2f},
		{4.2f, 17.8f}, {3, 17.8f}});
	b.poly({{11.5f, 1.5f}, {12.7f, 1.5f}, {12.7f, 5.3f}, {16.5f, 5.3f}, {16.5f, 6.5f}, {11.5f, 6.5f}});
});
const std::string alert_disc = make([](PathB& b) {
	b.box(16, 16);
	b.disc(8, 8, 8, 28);
});
const std::string play_disc = make([](PathB& b) {
	b.box(57, 57);
	b.ring(28.5f, 28.5f, 28.5f, 26);
	b.poly({{22, 17}, {41, 28.5f}, {22, 40}});
});
/* the filled speech bubble after the title: the count of unread chats beside it */
const std::string chat_badge = make([](PathB& b) {
	b.box(13, 12);
	b.rect(0, 0, 13, 9);
	b.poly({{4, 8.5f}, {8, 8.5f}, {5, 12}});
});
/* the voice note's play ring: 36 across with a 2.5 ring at that size */
const std::string ring = make([](PathB& b) {
	b.box(36, 36);
	b.ring(18, 18, 18, 15.5f);
});
const std::string star = make([](PathB& b) {
	b.box(16, 16);
	std::vector<std::pair<float, float>> p;
	for (int i = 0; i < 10; i++) {
		const float r = i % 2 ? 3.2f : 7.6f;
		const float a = (-90.0f + 36.0f * float(i)) * 3.14159265f / 180.0f;
		p.push_back({8 + r * std::cos(a), 8.4f + r * std::sin(a)});
	}
	b.poly(p);
});
}  // namespace glyph

ZColor literal(const uint32_t argb) { return Zcolor_argb(argb); }

Zid text(const std::string& s, const ZThemeResource size, const ZFontFamily family, const ZThemeResource fg) {
	const Zid t = Ztext_block_create(s.c_str());
	Ztext_block_bind_font_size(t, size);
	Ztext_block_set_font_family(t, family);
	if (fg != Z_RES_NONE) Ztext_block_bind_foreground(t, fg);
	return t;
}

Zid text_px(const std::string& s, const float size, const ZFontFamily family, const ZThemeResource fg) {
	const Zid t = Ztext_block_create(s.c_str());
	Ztext_block_set_font_size(t, size);
	Ztext_block_set_font_family(t, family);
	if (fg != Z_RES_NONE) Ztext_block_bind_foreground(t, fg);
	return t;
}

void set_trim(const Zid tb) {
	Ztext_block_set_text_wrapping(tb, Z_NO_WRAP);
	Ztext_block_set_text_trimming(tb, Z_TRIM_WORD_ELLIPSIS);
}

/* An icon takes the foreground of the control it is in: a ContentControl bound to a key gives it a live colour. */
Zid icon_holder(const Zid icon, const float size, const ZThemeResource fg) {
	const Zid c = Zcontent_control_create();
	Zelement_set_width(icon, size);
	Zelement_set_height(icon, size);
	Zcontent_control_set_content(c, icon);
	if (fg != Z_RES_NONE) Zcontrol_bind_foreground(c, fg);
	Zelement_set_width(c, size);
	Zelement_set_height(c, size);
	Zelement_set_hit_test_visible(c, false);
	return c;
}

Zid path_icon(const std::string& data) { return Zpath_icon_create(data.c_str()); }
Zid symbol(const ZSymbol s) { return Zsymbol_icon_create(s); }

/* A tappable area with no chrome of its own: a HyperlinkButton (tilt, no fill when pressed) holding the content. */
Zid tappable(const Zid content) {
	const Zid b = Zhyperlink_button_create(nullptr);
	Zcontrol_set_padding(b, Zthickness_uniform(0));
	Zcontrol_set_border_thickness(b, Zthickness_uniform(0));
	Zcontrol_set_horizontal_content_alignment(b, Z_HALIGN_STRETCH);
	Zcontrol_set_vertical_content_alignment(b, Z_VALIGN_STRETCH);
	Zcontent_control_set_content(b, content);
	Zelement_set_tilt(b, true);
	return b;
}

Zid border(const ZThemeResource background) {
	const Zid b = Zborder_create();
	if (background != Z_RES_NONE) Zborder_bind_background(b, background);
	return b;
}

Zid avatar(const std::string& jid, const std::string& name, const float size, const bool b_group) {
	const Zid b = border(Z_RES_PHONE_ACCENT_BRUSH);
	Zelement_set_width(b, size);
	Zelement_set_height(b, size);
	Zelement_set_clip_to_bounds(b, true);
	const Zid g = Zgrid_create();
	Zborder_set_child(b, g);
	if (b_group) {
		const Zid ic = icon_holder(path_icon(glyph::people), size >= 58 ? 28.0f : size * 0.5f, Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
		Zelement_set_horizontal_alignment(ic, Z_HALIGN_CENTER);
		Zelement_set_vertical_alignment(ic, Z_VALIGN_CENTER);
		Zpanel_add(g, ic);
	} else {
		const Zid m = text(first_letter(name), size >= 58 ? Z_RES_TEXT_STYLE_EXTRA_LARGE_FONT_SIZE : Z_RES_TEXT_STYLE_LARGE_FONT_SIZE,
			Z_FONT_SEMILIGHT, Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
		Zelement_set_horizontal_alignment(m, Z_HALIGN_CENTER);
		Zelement_set_vertical_alignment(m, Z_VALIGN_CENTER);
		Zpanel_add(g, m);
	}
	const Zid img = Zimage_create();
	Zimage_set_stretch(img, Z_STRETCH_UNIFORM_TO_FILL);
	Zelement_set_visibility(img, Z_COLLAPSED);
	Zpanel_add(g, img);
	if (!jid.empty()) picture_want(jid, img);
	return b;
}

/* Title (Bold upper case) and header (SemiLight lower case) where the main pivot draws them (wp:: in wa.h). */
Zid page_frame(const Zid page, const char* const title, const char* const header, Zid* const body) {
	const Zid g = Zgrid_create();
	Zgrid_add_row(g, Z_GRID_AUTO, 0);
	Zgrid_add_row(g, Z_GRID_AUTO, 0);
	Zgrid_add_row(g, Z_GRID_STAR, 1);
	Zpage_set_content(page, g);
	Zpanel_set_background(g, Zcolor_rgba(0, 0, 0, 0));
	if (title) {
		const Zid t = text_px(title, wp::TITLE_SIZE, Z_FONT_BOLD, Z_RES_PHONE_FOREGROUND_BRUSH);
		Zelement_set_margin(t, Zthickness(wp::LEFT, wp::TITLE_TOP, M, 0));
		set_trim(t);
		Zgrid_place(g, t, 0, 0);
	}
	if (header) {
		const Zid h = text_px(header, wp::HEADER_SIZE, Z_FONT_SEMILIGHT, Z_RES_PHONE_FOREGROUND_BRUSH);
		Ztext_block_bind_character_spacing(h, Z_RES_PIVOT_HEADER_ITEM_CHARACTER_SPACING);
		/* its line where the pivot's strip draws it: the strip top less the 6.5 the header's margin pulls up */
		const float title_line = title ? wp::TITLE_TOP + wp::TITLE_SIZE * 1.331f : 0.0f;
		Zelement_set_margin(h, Zthickness(wp::LEFT, wp::STRIP_TOP - 6.5f - title_line, M, 0));
		set_trim(h);
		Zgrid_place(g, h, 0, 1);
	}
	const Zid b = Zgrid_create();
	/* the body where a pivot item starts: under the header line (6.5 of it above the strip), plus the item's own top */
	Zelement_set_margin(b, Zthickness(wp::LEFT, header ? 6.5f + wp::ITEM_TOP : 9.5f, wp::RIGHT, 0));
	Zgrid_place(g, b, 0, 2);
	/* the connection banner overlays the top of the body (spec 6, 4.11) */
	const Zid bn = banner();
	Zgrid_place(g, bn, 0, 2);
	Zpanel_set_zindex(bn, 10);
	*body = b;
	return g;
}

Zid top_progress() {
	const Zid p = Zprogress_bar_create();
	Zprogress_bar_set_is_indeterminate(p, true);
	Zelement_set_height(p, 2);
	Zelement_set_vertical_alignment(p, Z_VALIGN_TOP);
	return p;
}

Zid empty_state(const char* const heading, const char* const line) {
	const Zid s = Zstack_panel_create(Z_VERTICAL);
	Zelement_set_margin(s, Zthickness(0, M, 0, M));
	const Zid h = text(heading, Z_RES_TEXT_STYLE_EXTRA_LARGE_FONT_SIZE, Z_FONT_SEMILIGHT, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
	Ztext_block_set_text_wrapping(h, Z_WRAP);
	Zpanel_add(s, h);
	if (line && *line) {
		const Zid l = text(line, Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
		Ztext_block_set_text_wrapping(l, Z_WRAP);
		Zpanel_add(s, l);
	}
	return s;
}

/* ---- the connection banner (spec 6): shown after 1.5 s of not connected, overlays, never shifts content ---- */
Zid banner() {
	const Zid b = border(Z_RES_PHONE_CHROME_COLOR);
	Zelement_set_height(b, 28.5f);
	Zelement_set_vertical_alignment(b, Z_VALIGN_TOP);
	const Zid g = Zgrid_create();
	Zborder_set_child(b, g);
	const Zid t = text("", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_FOREGROUND_BRUSH);
	Zelement_set_margin(t, Zthickness(M, 0, M, 0));
	Zelement_set_vertical_alignment(t, Z_VALIGN_CENTER);
	Zelement_set_name(t, "banner-text");
	Zpanel_add(g, t);
	const Zid bar = top_progress();
	Zelement_set_vertical_alignment(bar, Z_VALIGN_BOTTOM);
	Zelement_set_name(bar, "banner-bar");
	Zpanel_add(g, bar);
	Zelement_set_visibility(b, Z_COLLAPSED);
	Zelement_set_hit_test_visible(b, false);
	g_banners.push_back(b);
	g_banner_was.clear();
	post([] { banners_update(); });
	return b;
}

void banners_update() {
	std::string line;
	bool b_bar = false;
	if (!engine_up()) line = "WhatsApp service not running";
	else if (st.connection == "offline") line = "waiting for network";
	else if (st.connection == "connecting") {
		line = "connecting...";
		b_bar = true;
	}
	/* unpaired or logged out shows the link page instead */
	if (st.pair_state == "unpaired" || st.pair_state == "qr" || st.pair_state == "phone_code" || st.b_logged_out) line.clear();
	if (line.empty()) {
		cancel(g_banner_timer);
		g_banner_timer = 0;
		g_banner_on = false;
	} else if (!g_banner_on && !g_banner_timer) {
		g_banner_timer = after(1500, [] {
			g_banner_timer = 0;
			g_banner_on = true;
			g_banner_was.clear();
			banners_update();
		});
	}
	const bool b_show = g_banner_on && !line.empty();
	const std::string key = (b_show ? "1" : "0") + line;
	std::vector<Zid> live;
	for (const Zid b : g_banners) {
		if (!Zelement_is_valid(b)) continue;
		live.push_back(b);
		const bool b_was = Zelement_get_visibility(b) == Z_VISIBLE;
		Ztext_block_set_text(Zelement_find_name(b, "banner-text"), line.c_str());
		Zelement_set_visibility(Zelement_find_name(b, "banner-bar"), b_bar ? Z_VISIBLE : Z_COLLAPSED);
		if (b_show && !b_was) {
			Zelement_set_visibility(b, Z_VISIBLE);
			Zelement_set_translate(b, 0, -28.5f);
			Zelement_set_opacity(b, 0);
			Zelement_animate(b, Z_PROP_TRANSLATE_Y, 0, 200, Z_EASE_OUT, 0);
			Zelement_animate(b, Z_PROP_OPACITY, 1, 200, Z_EASE_OUT, 0);
		} else if (!b_show && b_was) {
			Zelement_animate(b, Z_PROP_TRANSLATE_Y, -28.5f, 200, Z_EASE_OUT, 0);
			Zelement_animate(b, Z_PROP_OPACITY, 0, 200, Z_EASE_OUT, 0);
			after(210, [b] {
				if (Zelement_is_valid(b) && !g_banner_on) Zelement_set_visibility(b, Z_COLLAPSED);
			});
		}
	}
	g_banners.swap(live);
	g_banner_was = key;
}

/* ---- app bar ---- */
Zid app_bar() {
	const Zid bar = Zcommand_bar_create();
	Zapp_bar_set_closed_display_mode(bar, Z_APP_BAR_CLOSED_COMPACT);
	/* zrn-frosted-diffusion: the bar is frosted glass in the chrome's tint (what scrolls behind it shows through blurred and
	 * grainy, its buttons as sharp as on the solid bar); the page lays its content out under it and its lists keep room for it */
	Zelement_set_frosted_diffusion(bar, Z_FROSTED_DIFFUSION_NORMAL, Z_RES_PHONE_CHROME_COLOR);
	return bar;
}

Zid bar_button(const Zid bar, const char* const label, const Zid icon, std::function<void()> fn, const bool b_secondary) {
	const Zid b = Zapp_bar_button_create(label, icon);
	click(b, std::move(fn));
	if (b_secondary) Zcommand_bar_add_secondary_command(bar, b);
	else Zcommand_bar_add_primary_command(bar, b);
	return b;
}

Zid bar_secondary(const Zid bar, const char* const label, std::function<void()> fn) {
	return bar_button(bar, label, Z_INVALID_ID, std::move(fn), true);
}

void bar_enable(const Zid bar, const bool b_on) {
	if (!Zelement_is_valid(bar)) return;
	for (int i = 0; i < Zcommand_bar_get_primary_command_count(bar); i++) {
		const Zid c = Zcommand_bar_get_primary_command(bar, i);
		if (Zelement_get_tag(c) == reinterpret_cast<void*>(1)) continue; /* kept off by its page */
		Zelement_set_enabled(c, b_on);
	}
}

Zid menu_item(const Zid menu, const char* const label, std::function<void()> fn, const bool b_enabled) {
	const Zid it = Zmenu_flyout_item_create(label);
	sweep();
	g_handlers[it] = std::move(fn);
	Zmenu_flyout_item_on_click(it, trampoline, nullptr);
	Zelement_set_enabled(it, b_enabled);
	Zmenu_flyout_add_item(menu, it);
	return it;
}

void confirm(const char* const title, const char* const content, const char* const primary, std::function<void()> yes) {
	const Zid d = Zcontent_dialog_create();
	Zcontent_dialog_set_title(d, title);
	if (content) Zcontent_control_set_text(d, content);
	Zcontent_dialog_set_primary_button_text(d, primary);
	Zcontent_dialog_set_secondary_button_text(d, "cancel");
	auto* fn = new std::function<void()>(std::move(yes));
	Zcontent_dialog_show(d, g_root, [](const Zid dialog, const ZContentDialogResult r, void* const ud) {
		auto* f = static_cast<std::function<void()>*>(ud);
		if (r == Z_CONTENT_DIALOG_RESULT_PRIMARY) (*f)();
		delete f;
		post([dialog] { Zelement_destroy(dialog); });
	}, fn);
}

void click(const Zid button, std::function<void()> fn) {
	sweep();
	g_handlers[button] = std::move(fn);
	Zbutton_on_click(button, trampoline, nullptr);
}

void on_text(const Zid text_box, std::function<void()> fn) {
	g_text_handlers[text_box] = std::move(fn);
	Zelement_on(text_box, Z_EVENT_TEXT_CHANGED, text_trampoline, nullptr);
}

/* Spec 4.3: the rows in view swing in round their left edge one after another; the rest just appear. */
void list_entrance(const Zid list) {
	if (!Zroot_animations_enabled(g_root)) return;
	Zroot_update_layout(g_root);
	const Zid sv = Zitems_control_get_scroll_viewer(list);
	const double top = Zscroll_viewer_get_vertical_offset(sv);
	const double view = Zscroll_viewer_get_viewport_height(sv);
	const int n = Zitems_control_get_count(list);
	int shown = 0;
	for (int i = 0; i < n && shown < 10; i++) {
		const Zid c = Zitems_control_get_container(list, i);
		const ZRect r = Zelement_get_bounds(c);
		if (r.y + r.height < top) continue;
		if (r.y > top + (view > 0 ? view : 800)) break;
		Zelement_set_projection(c, 0, -60, 0, 0.5f);
		Zelement_set_opacity(c, 0);
		Zelement_animate(c, Z_PROP_ROTATION_Y, 0, 300, Z_EASE_EMPHASIZED, 30.0f * float(shown));
		Zelement_animate(c, Z_PROP_OPACITY, 1, 300, Z_EASE_EMPHASIZED, 30.0f * float(shown));
		shown++;
	}
}

void row_flash(const Zid row) {
	Zelement_animate(row, Z_PROP_OPACITY, 0.5f, 167, Z_EASE_OUT, 0);
	Zelement_animate(row, Z_PROP_OPACITY, 1.0f, 167, Z_EASE_OUT, 167);
}

/* ---- theme: the shell's theme and accent, live (G1 shim: the app reads the shell's config and watches it) ---- */
void theme_apply() {
	static const struct {
		const char* name;
		ZAccent accent;
	} accents[] = {{"lime", Z_ACCENT_LIME}, {"green", Z_ACCENT_GREEN}, {"emerald", Z_ACCENT_EMERALD}, {"teal", Z_ACCENT_TEAL},
		{"cyan", Z_ACCENT_CYAN}, {"cobalt", Z_ACCENT_COBALT}, {"indigo", Z_ACCENT_INDIGO}, {"violet", Z_ACCENT_VIOLET},
		{"pink", Z_ACCENT_PINK}, {"magenta", Z_ACCENT_MAGENTA}, {"crimson", Z_ACCENT_CRIMSON}, {"red", Z_ACCENT_RED},
		{"orange", Z_ACCENT_ORANGE}, {"amber", Z_ACCENT_AMBER}, {"yellow", Z_ACCENT_YELLOW}, {"brown", Z_ACCENT_BROWN},
		{"olive", Z_ACCENT_OLIVE}, {"steel", Z_ACCENT_STEEL}, {"mauve", Z_ACCENT_MAUVE}, {"taupe", Z_ACCENT_TAUPE}};
	static bool b_first = true;
	ZTheme theme = Z_THEME_DARK;
	ZAccent accent = Z_ACCENT_COBALT;
	bool b_found = false;
	const char* home = getenv("HOME");
	if (FILE* f = fopen((std::string(home ? home : "") + "/.config/ztmetro/config").c_str(), "r")) {
		char line[256];
		char value[32];
		while (fgets(line, sizeof line, f)) {
			if (sscanf(line, " theme = %31s", value) == 1) {
				theme = !strcmp(value, "light") ? Z_THEME_LIGHT : Z_THEME_DARK;
				b_found = true;
			}
			if (sscanf(line, " accent = %31s", value) == 1)
				for (const auto& a : accents)
					if (!strcmp(a.name, value)) accent = a.accent;
		}
		fclose(f);
	}
	/* the session value wins at start, as the shell's own apps do; later edits of the config win over it */
	if (b_first) {
		if (const char* s = getenv("ZTRF_SHELL_THEME")) theme = !strcmp(s, "light") ? Z_THEME_LIGHT : Z_THEME_DARK;
		b_first = false;
	}
	(void)b_found;
	/* with G1 zrnapp follows the shell's theme itself; the app only lays its green over the accent */
	if (!gap::have_theme_follow() && Zroot_get_theme(g_root) != theme) Zroot_set_theme(g_root, theme);
	/* the pivot of the WhatsApp page: no pivot title (the page draws its own), the strip and the headers' size measured */
	Zroot_set_resource_thickness(g_root, Z_RES_PIVOT_PORTRAIT_THEME_PADDING, Zthickness(wp::LEFT, wp::STRIP_TOP, 0, 0));
	Zroot_set_resource_double(g_root, Z_RES_PIVOT_HEADER_ITEM_FONT_SIZE, wp::HEADER_SIZE);
	if (st.prefs.b_green) Zroot_set_accent_color(g_root, literal(0xFF25D366)); /* L3 */
	else Zroot_set_accent(g_root, accent);
}

}  // namespace wa
