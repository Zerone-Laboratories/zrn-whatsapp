// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * S2 the chat (spec 2.2), its bubbles (3), list items (3.4), animations (4.6-4.9) and S14 the emoji and stickers panel
 * (2.11), which takes the keyboard's place under the composer. Voice notes (hold the mic, slide to cancel), documents,
 * stickers, and swipe-right-to-reply. A bubble is built once per message and changed in place (ticks, edits, the voice
 * player); rows are never measured by guess.
 */
#include "wa.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <set>
#include <unistd.h>

namespace wa {
namespace {

/* the bubbles of the phone app (docs/REFERENCE-MEASUREMENTS.md): 271.5 wide at most, 18.5 from the left and 20 from the right
 * edge of the screen, 50 from the other side; incoming in the accent, outgoing in the accent a third darker */
constexpr float BUBBLE_MAX = 271.5f;
constexpr float LIST_LEFT = 18.5f, LIST_RIGHT = 20.0f, BUBBLE_AWAY = 50.0f;
constexpr float TAIL_W = 10.0f, TAIL_H = 7.0f, TAIL_IN_X = 9.0f, TAIL_OUT_X = 9.0f;
constexpr float BODY_SIZE = 16.0f, BODY_LINE = 20.0f, SENDER_SIZE = 13.0f, META_SIZE = 12.5f;
/* the darkening of an outgoing bubble: black at a third over the accent (0,171,171 -> 0,114,114 on the phone) */
constexpr float OUT_SHADE = 1.0f / 3.0f;
constexpr float IMAGE_W = 236.0f;
/* incoming: above the bubble's left edge, its upright side on the edge (spec 3.1); outgoing: below the right edge, upright side on the edge */
const char* const TAIL_IN = "M0,7 L0,0 L10,7 Z";
const char* const TAIL_OUT = "M0,0 L10,0 L10,7 Z";

struct Row {
	std::string id;
	Zid container = Z_INVALID_ID, date = Z_INVALID_ID, block = Z_INVALID_ID, tail_top = Z_INVALID_ID, tail_bottom = Z_INVALID_ID,
		sender = Z_INVALID_ID, body = Z_INVALID_ID, edited = Z_INVALID_ID, tick = Z_INVALID_ID, tick_icon = Z_INVALID_ID,
		failed = Z_INVALID_ID, full_image = Z_INVALID_ID, swipe_icon = Z_INVALID_ID, play = Z_INVALID_ID, dur = Z_INVALID_ID,
		voice_mic = Z_INVALID_ID, doc_sub = Z_INVALID_ID;
	std::vector<Zid> bars; /* the voice note's waveform */
	bool b_swiping = false;
};

struct ChatView {
	std::string id, name;
	bool b_group = false, b_channel = false;
	std::deque<Msg> msgs;
	std::deque<Row> rows; /* parallel to msgs */
	bool b_more = false, b_older_pending = false, b_loaded = false;
	/* the first page came while the page was not laid out yet (held back by the turnstile): the landing waits for it */
	bool b_land_pending = false;
	/* a message came while another page was over the chat (the file picker): the list goes to its end when the chat is back */
	bool b_end_pending = false;
	Zid page = Z_INVALID_ID, list = Z_INVALID_ID, sv = Z_INVALID_ID, panel = Z_INVALID_ID, box = Z_INVALID_ID, send = Z_INVALID_ID,
		status = Z_INVALID_ID, progress = Z_INVALID_ID, older = Z_INVALID_ID, strip = Z_INVALID_ID, strip_label = Z_INVALID_ID,
		strip_quote = Z_INVALID_ID, jump = Z_INVALID_ID, badge = Z_INVALID_ID, badge_text = Z_INVALID_ID, divider = Z_INVALID_ID,
		typing = Z_INVALID_ID, composer = Z_INVALID_ID, header = Z_INVALID_ID, bar = Z_INVALID_ID, area = Z_INVALID_ID,
		/* the room of the app bar under the composer: the page gives a bar with a backdrop none of its own */
		bar_room = Z_INVALID_ID,
		/* the emoji / sticker panel in the keyboard's place */
		pane = Z_INVALID_ID, pane_emoji = Z_INVALID_ID, pane_stickers = Z_INVALID_ID, tab_emoji = Z_INVALID_ID, tab_stickers = Z_INVALID_ID,
		emoji_icon = Z_INVALID_ID, emoji_btn = Z_INVALID_ID,
		/* recording a voice note */
		mic = Z_INVALID_ID, rec_bar = Z_INVALID_ID, rec_time = Z_INVALID_ID, rec_dot = Z_INVALID_ID, rec_hint = Z_INVALID_ID;
	bool b_pane = false, b_rec = false, b_rec_cancelled = false;
	float rec_dx = 0;
	int rec_timer = 0;
	std::map<std::string, double> voice_pos; /* where each note was paused */
	/* the voice note whose waveform the finger is on: the player's ticks do not move its position meanwhile */
	std::string seek_id;
	std::string reply_id, edit_id, members;
	int unseen = 0, typing_timer = 0, local = 0;
	long last_typing = 0;
	std::string last_text;
} cv;

bool alive() { return cv.page != Z_INVALID_ID && Zelement_is_valid(cv.page); }

int index_of(const std::string& id) {
	for (int i = int(cv.msgs.size()) - 1; i >= 0; i--)
		if (cv.msgs[size_t(i)].id == id) return i;
	return -1;
}

bool same_day(const long a, const long b) {
	const time_t ta = a, tb = b;
	struct tm x;
	struct tm y;
	localtime_r(&ta, &x);
	localtime_r(&tb, &y);
	return x.tm_yday == y.tm_yday && x.tm_year == y.tm_year;
}

bool bubbled(const Msg& m) { return m.kind != "system" && m.kind != "sticker"; }

/* consecutive messages of one sender within 60 s are a group (spec 3.1) */
bool grouped(const Msg& a, const Msg& b) {
	return bubbled(a) && bubbled(b) && a.b_from_me == b.b_from_me && a.sender == b.sender && std::labs(b.ts - a.ts) <= 60 && same_day(a.ts, b.ts);
}

/* both bubbles are the accent: their text is the text over the accent, the time and the sender dimmed by opacity */
ZThemeResource fg_of(const Msg&) { return Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR; }
ZThemeResource meta_of(const Msg&) { return Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR; }

/* a tail in the bubble's colour: the accent, and for an outgoing one the shade over it */
Zid tail_icon(const char* const path, const bool b_out) {
	const Zid g = Zgrid_create();
	Zelement_set_width(g, TAIL_W);
	Zelement_set_height(g, TAIL_H);
	const Zid a = icon_holder(path_icon(path), TAIL_W, Z_RES_PHONE_ACCENT_BRUSH);
	Zelement_set_height(a, TAIL_H);
	Zelement_set_height(Zcontent_control_get_content(a), TAIL_H);
	Zpanel_add(g, a);
	if (b_out) {
		const Zid s = path_icon(path);
		Zicon_element_set_foreground(s, Zcolor_rgba(0, 0, 0, OUT_SHADE));
		Zelement_set_width(s, TAIL_W);
		Zelement_set_height(s, TAIL_H);
		Zpanel_add(g, s);
	}
	Zelement_set_hit_test_visible(g, false);
	return g;
}

/* ---- ticks (spec 2.3): drawn glyphs, 16 x 11 ---- */
void tick_set(Row& r, const Msg& m) {
	if (!Zelement_is_valid(r.tick)) return;
	const bool b_failed = m.status == "failed";
	Zelement_set_visibility(r.tick, b_failed ? Z_COLLAPSED : Z_VISIBLE);
	if (Zelement_is_valid(r.failed)) Zelement_set_visibility(r.failed, b_failed ? Z_VISIBLE : Z_COLLAPSED);
	const std::string& data = m.status == "pending" ? glyph::tick_clock : m.status == "sent" ? glyph::tick_one : glyph::tick_two;
	Zpath_icon_set_data(r.tick_icon, data.c_str());
	/* in the accent bubble: text over accent, 0.7 for the dim states, 1.0 read */
	const float want = m.status == "read" ? 1.0f : 0.7f;
	if (std::fabs(Zelement_get_opacity(r.tick) - want) > 0.01f) {
		Zelement_set_opacity(r.tick, 0.2f);
		Zelement_animate(r.tick, Z_PROP_OPACITY, want, 120, Z_EASE_LINEAR, 0);
	}
}

Zid make_tick(Row& r, const Msg& m, const bool b_on_chip = false) {
	const Zid s = Zgrid_create();
	r.tick_icon = path_icon(glyph::tick_clock);
	Zelement_set_width(r.tick_icon, 16);
	Zelement_set_height(r.tick_icon, 11);
	r.tick = Zcontent_control_create();
	Zcontent_control_set_content(r.tick, r.tick_icon);
	/* over the accent bubble always white; on the chip of an image or a sticker (dimming of the theme) the theme's ink */
	Zcontrol_bind_foreground(r.tick, b_on_chip ? Z_RES_PHONE_FOREGROUND_BRUSH : Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
	Zelement_set_opacity(r.tick, 0.7f);
	Zelement_set_vertical_alignment(r.tick, Z_VALIGN_CENTER);
	Zpanel_add(s, r.tick);
	/* failed: the red disc with "!" (L1) */
	r.failed = Zgrid_create();
	const Zid disc = path_icon(glyph::alert_disc);
	Zicon_element_set_foreground(disc, literal(0xFFE51400));
	Zelement_set_width(disc, 16);
	Zelement_set_height(disc, 16);
	Zpanel_add(r.failed, disc);
	const Zid bang = text_px("!", 13, Z_FONT_SEMIBOLD, Z_RES_NONE);
	Ztext_block_set_foreground(bang, literal(0xFFFFFFFF));
	Zelement_set_horizontal_alignment(bang, Z_HALIGN_CENTER);
	Zelement_set_vertical_alignment(bang, Z_VALIGN_CENTER);
	Zpanel_add(r.failed, bang);
	Zpanel_add(s, r.failed);
	Zelement_set_margin(s, Zthickness(H, 0, 0, 0));
	/* a zrnui Grid with room to fill takes all of it: in the chip over an image it would be the image's height */
	Zelement_set_height(s, 16);
	Zelement_set_vertical_alignment(s, Z_VALIGN_CENTER);
	tick_set(r, m);
	return s;
}

Zid meta_line(Row& r, const Msg& m, const bool b_on_chip = false) {
	const ZThemeResource ink = b_on_chip ? Z_RES_PHONE_FOREGROUND_BRUSH : meta_of(m);
	const Zid s = Zstack_panel_create(Z_HORIZONTAL);
	Zelement_set_horizontal_alignment(s, Z_HALIGN_RIGHT);
	r.edited = text_px("edited", META_SIZE, Z_FONT_NORMAL, ink);
	if (!b_on_chip) Zelement_set_opacity(r.edited, 0.5f);
	Zelement_set_margin(r.edited, Zthickness(0, 0, H, 0));
	Zelement_set_visibility(r.edited, m.b_edited ? Z_VISIBLE : Z_COLLAPSED);
	Zpanel_add(s, r.edited);
	if (m.b_starred) {
		const Zid star = icon_holder(path_icon(glyph::star), 13, ink);
		Zelement_set_vertical_alignment(star, Z_VALIGN_CENTER);
		Zelement_set_margin(star, Zthickness(0, 0, H, 0));
		Zpanel_add(s, star);
	}
	const Zid t = text_px(time_hm(m.ts), META_SIZE, Z_FONT_NORMAL, ink);
	if (!b_on_chip) Zelement_set_opacity(t, 0.5f);
	Zpanel_add(s, t);
	if (m.b_from_me && !m.b_deleted) Zpanel_add(s, make_tick(r, m, b_on_chip));
	return s;
}

const Msg* find_msg(const std::string& id) {
	const int i = index_of(id);
	return i >= 0 ? &cv.msgs[size_t(i)] : nullptr;
}

void scroll_center(int index);
std::string label_of(const Msg& m);

Zid reply_quote(const Msg& m) {
	const Msg* q = find_msg(m.quoted_id);
	const Zid g = Zgrid_create();
	Zgrid_add_column(g, Z_GRID_PIXEL, H);
	Zgrid_add_column(g, Z_GRID_STAR, 1);
	Zpanel_set_background(g, Zcolor_rgba(0, 0, 0, 0));
	const Zid back = border(Z_RES_PHONE_BASE_LOW_COLOR);
	Zgrid_place(g, back, 0, 0);
	Zgrid_set_column_span(back, 2);
	const Zid bar = border(m.b_from_me ? Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR : Z_RES_PHONE_ACCENT_BRUSH);
	Zgrid_place(g, bar, 0, 0);
	const Zid col = Zstack_panel_create(Z_VERTICAL);
	Zelement_set_margin(col, Zthickness(G, H, G, H));
	const std::string who = q ? (q->b_from_me ? "you" : q->sender_name) : "message";
	const Zid s = text(who, Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_SEMIBOLD, m.b_from_me ? Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR : Z_RES_PHONE_ACCENT_BRUSH);
	set_trim(s);
	Zpanel_add(col, s);
	const Zid t = text(q ? label_of(*q) : "", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, fg_of(m));
	set_trim(t);
	Zelement_set_opacity(t, 0.8f);
	Zpanel_add(col, t);
	Zgrid_place(g, col, 1, 0);
	const Zid tap = tappable(g);
	Zelement_set_margin(tap, Zthickness(0, 0, 0, H));
	const std::string qid = m.quoted_id;
	click(tap, [qid] {
		const int i = index_of(qid);
		if (i >= 0) scroll_center(i);
	});
	return tap;
}

void open_media(const Msg& m, const PixP& thumb) {
	g_media_view = {m.chat.empty() ? cv.id : m.chat, m.id, m.media.path, m.text, thumb};
	nav(page_media, "", gap::slide_up());
}

/* the box of a picture in a bubble, in points */
void image_box(const Msg& m, float* const w, float* const h) {
	*w = IMAGE_W;
	const float aspect = m.media.width > 0 && m.media.height > 0 ? float(m.media.height) / float(m.media.width) : 0.75f;
	*h = std::clamp(*w * aspect, 95.0f, 304.0f);
}

/* the full picture into its row's box: decoded on the worker at the box's device size, then faded in over the thumb */
void image_show_full(const std::string& id) {
	const int i = index_of(id);
	if (i < 0) return;
	const Msg& m = cv.msgs[size_t(i)];
	const Zid full = cv.rows[size_t(i)].full_image;
	if (m.media.path.empty() || !Zelement_is_valid(full)) return;
	float w, h;
	image_box(m, &w, &h);
	image_file(m.media.path, w, h, true, [full](PixP p) {
		if (!p || !Zelement_is_valid(full)) return;
		image_set(full, p);
		Zelement_animate(full, Z_PROP_OPACITY, 1, 167, Z_EASE_LINEAR, 0);
	});
}

/* Pictures come in full by themselves on Wi-Fi or USB (as WhatsApp's media auto-download), else when tapped: the
 * thumb a message carries is a few dozen pixels, blurred at a bubble's size (QA: photos were blurry). Two at a time,
 * newest first. */
std::deque<std::string> g_img_queue;
std::set<std::string> g_img_asked;
int g_img_busy;

void image_fetch_next() {
	while (g_img_busy < 2 && !g_img_queue.empty() && alive()) {
		const std::string id = g_img_queue.back();
		g_img_queue.pop_back();
		if (index_of(id) < 0) continue;
		g_img_busy++;
		const std::string chat = cv.id;
		request("download_media", {{"chat", chat}, {"message_id", id}}, [id, chat](const json* res, const char* err) {
			g_img_busy--;
			const std::string p = err ? "" : jstr(*res, "path");
			if (p.empty()) g_img_asked.erase(id); /* tried again when the row is made again or tapped */
			else if (alive() && cv.id == chat) {
				const int k = index_of(id);
				if (k >= 0) {
					cv.msgs[size_t(k)].media.path = p;
					image_show_full(id);
				}
			}
			image_fetch_next();
		});
	}
}

void image_fetch(const std::string& id) {
	if (!g_img_asked.insert(id).second) return;
	g_img_queue.push_back(id);
	post([] { image_fetch_next(); });
}

/* image (spec 3.3): thumb at once, full picture cross-faded into the same box */
Zid image_part(Row& r, const Msg& m, const bool b_only) {
	float w, h;
	image_box(m, &w, &h);
	const Zid g = Zgrid_create();
	Zelement_set_width(g, w);
	Zelement_set_height(g, h);
	Zelement_set_clip_to_bounds(g, true);
	const Zid base = border(Z_RES_PHONE_BASE_LOW_COLOR);
	Zpanel_add(g, base);
	const Zid thumb = Zimage_create();
	Zimage_set_stretch(thumb, Z_STRETCH_UNIFORM_TO_FILL);
	Zpanel_add(g, thumb);
	r.full_image = Zimage_create();
	Zimage_set_stretch(r.full_image, Z_STRETCH_UNIFORM_TO_FILL);
	Zelement_set_opacity(r.full_image, 0);
	Zpanel_add(g, r.full_image);
	auto thumb_pix = std::make_shared<PixP>();
	if (!m.media.thumb.empty())
		image_bytes(unbase64(m.media.thumb), w, h, [thumb, thumb_pix](PixP p) {
			*thumb_pix = p;
			image_set(thumb, p);
		});
	if (!m.media.path.empty() && access(m.media.path.c_str(), R_OK) == 0) {
		const Zid full = r.full_image;
		image_file(m.media.path, w, h, true, [full](PixP p) {
			image_set(full, p);
			if (Zelement_is_valid(full)) Zelement_animate(full, Z_PROP_OPACITY, 1, 167, Z_EASE_LINEAR, 0);
		});
	} else if (!m.b_deleted && unmetered()) {
		image_fetch(m.id);
	}
	if (b_only) {
		const Zid chip = border(Z_RES_CONTENT_DIALOG_DIMMING_COLOR);
		Zborder_set_padding(chip, Zthickness_uniform(H));
		Zelement_set_horizontal_alignment(chip, Z_HALIGN_RIGHT);
		Zelement_set_vertical_alignment(chip, Z_VALIGN_BOTTOM);
		Zborder_set_child(chip, meta_line(r, m, true));
		Zpanel_add(g, chip);
	}
	const Zid tap = tappable(g);
	const Msg copy = m;
	click(tap, [copy, thumb_pix] { open_media(copy, *thumb_pix); });
	return tap;
}

Zid kind_row(const Msg& m) {
	const Zid s = Zstack_panel_create(Z_HORIZONTAL);
	const Zid ic = icon_holder(path_icon(glyph::page), 20, fg_of(m));
	Zelement_set_vertical_alignment(ic, Z_VALIGN_CENTER);
	Zelement_set_margin(ic, Zthickness(0, 0, G, 0));
	Zpanel_add(s, ic);
	std::string label = m.kind == "video" ? "video" : !m.media.file_name.empty() ? m.media.file_name : m.text.empty() ? m.kind : m.text;
	const Zid t = text(label, Z_RES_TEXT_STYLE_LARGE_FONT_SIZE, Z_FONT_NORMAL, fg_of(m));
	set_trim(t);
	Zpanel_add(s, t);
	return s;
}

/* what a message is, in a line (reply quotes, the reply strip) */
std::string label_of(const Msg& m) {
	if (!m.text.empty()) return one_line(m.text);
	if (m.kind == "audio") return m.media.b_ptt || m.media.seconds ? "voice message (" + std::to_string(m.media.seconds / 60) + ":" + (m.media.seconds % 60 < 10 ? "0" : "") + std::to_string(m.media.seconds % 60) + ")" : "audio";
	if (m.kind == "document") return m.media.file_name.empty() ? "document" : m.media.file_name;
	if (m.kind == "image") return "photo";
	return m.kind;
}

std::string mmss(const double sec) {
	const int s = std::max(0, int(sec + 0.5));
	char b[16];
	snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
	return b;
}

/* ---- voice notes (spec 3.3 audio): play/pause, the waveform as the seek bar, the length, the played mic ---- */
const char* const PLAY_PATH = "M0,0 L12,7 L0,14 Z";
const char* const PAUSE_PATH = "M0,0 L4,0 L4,14 L0,14 Z M8,0 L12,0 L12,14 L8,14 Z";
constexpr double VOICE_TRACK_W = 128.0; /* the voice note's line (voice_part) */

Row* row_of(const std::string& id) {
	const int i = index_of(id);
	return i >= 0 && size_t(i) < cv.rows.size() ? &cv.rows[size_t(i)] : nullptr;
}

void voice_show(Row& r, const Msg& m, const double pos, const double dur, const bool b_playing) {
	if (!Zelement_is_valid(r.play)) return;
	Zpath_icon_set_data(r.play, b_playing ? PAUSE_PATH : PLAY_PATH);
	const double total = dur > 0 ? dur : double(m.media.seconds);
	const double f = total > 0 ? std::clamp(pos / total, 0.0, 1.0) : 0.0;
	if (!r.bars.empty()) Zelement_set_width(r.bars[0], float(VOICE_TRACK_W * ((b_playing || pos > 0) ? f : 0.0)));
	Ztext_block_set_text(r.dur, mmss(b_playing || pos > 0 ? pos : total).c_str());
	/* the mic: green until an incoming note is heard, blue after; on an outgoing note the text colour, dimmed */
	if (Zelement_is_valid(r.voice_mic)) {
		if (m.b_from_me) {
			Zcontrol_bind_foreground(r.voice_mic, Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
			Zelement_set_opacity(r.voice_mic, 0.6f);
		} else {
			Zcontrol_set_foreground(r.voice_mic, literal(m.media.b_played ? 0xFF1BA1E2 : 0xFF6CC04A));
		}
	}
}

void voice_play(const std::string& id, double from);

void voice_tick(const std::string& id, const double pos, const double dur, const bool b_playing) {
	if (!alive() || cv.seek_id == id) return;
	const int i = index_of(id);
	if (i < 0) return;
	cv.voice_pos[id] = b_playing || pos < dur - 0.1 ? pos : 0;
	voice_show(cv.rows[size_t(i)], cv.msgs[size_t(i)], b_playing || pos < dur - 0.1 ? pos : 0, dur, b_playing);
}

void voice_play(const std::string& id, const double from) {
	const int i = index_of(id);
	if (i < 0) return;
	Msg& m = cv.msgs[size_t(i)];
	if (!m.b_from_me && !m.media.b_played) {
		m.media.b_played = true;
		request("media_played", {{"chat", cv.id}, {"message_id", id}});
	}
	auto start = [id, from](const std::string& path) {
		voice::play(id, path, from, [id](const double pos, const double dur, const bool b_playing) { voice_tick(id, pos, dur, b_playing); });
	};
	if (!m.media.path.empty()) {
		start(m.media.path);
		return;
	}
	Row& r = cv.rows[size_t(i)];
	Ztext_block_set_text(r.dur, "...");
	request("download_media", {{"chat", cv.id}, {"message_id", id}}, [id, start](const json* res, const char* err) {
		if (!alive()) return;
		const int k = index_of(id);
		if (k < 0) return;
		const std::string p = err ? "" : jstr(*res, "path");
		if (p.empty()) {
			Ztext_block_set_text(cv.rows[size_t(k)].dur, "tap to retry");
			return;
		}
		cv.msgs[size_t(k)].media.path = p;
		start(p);
	});
}

void voice_toggle(const std::string& id) {
	if (voice::playing(id)) {
		voice::pause();
		return;
	}
	voice_play(id, cv.voice_pos.count(id) ? cv.voice_pos[id] : 0);
}

/* The voice note of the phone app (docs/REFERENCE-MEASUREMENTS.md): a 36 point ring with the play glyph, a 2 point line that
 * fills as it plays, the time under the line's end, the mic (green until an incoming note is heard, blue after) and the
 * speaker's 67 point picture with the length over its corner; the picture on the outer side, the ring toward the middle. */
constexpr float VOICE_H = 68.0f, VOICE_RING = 36.0f, VOICE_TRACK = 128.0f, VOICE_PIC = 67.0f;
Zid voice_part(Row& r, const Msg& m) {
	const bool b_out = m.b_from_me;
	const Zid g = Zgrid_create();
	Zgrid_add_row(g, Z_GRID_PIXEL, VOICE_H);
	if (b_out) Zgrid_add_column(g, Z_GRID_AUTO, 0);
	Zgrid_add_column(g, Z_GRID_AUTO, 0);
	Zgrid_add_column(g, Z_GRID_PIXEL, VOICE_TRACK);
	Zgrid_add_column(g, Z_GRID_AUTO, 0);
	if (!b_out) Zgrid_add_column(g, Z_GRID_AUTO, 0);
	const int c_ring = b_out ? 1 : 0, c_track = c_ring + 1, c_mic = c_track + 1, c_pic = b_out ? 0 : 3;
	/* play / pause: the ring, its glyph inside */
	const Zid circle = Zgrid_create();
	Zelement_set_width(circle, VOICE_RING);
	Zelement_set_height(circle, VOICE_RING);
	{
		/* the ring: a disc of the text colour, and over it a disc of the bubble's colour 2.5 smaller all round */
		const float R = VOICE_RING / 2, ri = R - 2.5f;
		const Zid outer = border(Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
		Zborder_set_corner_radius(outer, {R, R, R, R});
		Zpanel_add(circle, outer);
		const Zid inner = border(Z_RES_PHONE_ACCENT_BRUSH);
		Zborder_set_corner_radius(inner, {ri, ri, ri, ri});
		Zelement_set_width(inner, 2 * ri);
		Zelement_set_height(inner, 2 * ri);
		Zelement_set_horizontal_alignment(inner, Z_HALIGN_CENTER);
		Zelement_set_vertical_alignment(inner, Z_VALIGN_CENTER);
		Zelement_set_clip_to_bounds(inner, true);
		if (b_out) {
			const Zid sh = border(Z_RES_NONE);
			Zborder_set_background(sh, Zcolor_rgba(0, 0, 0, OUT_SHADE));
			Zborder_set_corner_radius(sh, {ri, ri, ri, ri});
			Zborder_set_child(inner, sh);
		}
		Zpanel_add(circle, inner);
	}
	r.play = path_icon(PLAY_PATH);
	const Zid ic = icon_holder(r.play, 14, Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
	Zelement_set_horizontal_alignment(ic, Z_HALIGN_CENTER);
	Zelement_set_vertical_alignment(ic, Z_VALIGN_CENTER);
	Zelement_set_margin(ic, Zthickness(3, 0, 0, 0)); /* the triangle's ink sits right of its box's middle */
	Zpanel_add(circle, ic);
	const Zid pb = tappable(circle);
	Zelement_set_vertical_alignment(pb, Z_VALIGN_TOP);
	Zelement_set_margin(pb, b_out ? Zthickness(12, 24, 9, 0) : Zthickness(7, 24, 9, 0));
	const std::string id = m.id;
	click(pb, [id] { voice_toggle(id); });
	Zgrid_place(g, pb, c_ring, 0);
	/* the line: 30 % of the text colour, the played part in full */
	const Zid track = Zgrid_create();
	Zelement_set_height(track, 2);
	Zelement_set_vertical_alignment(track, Z_VALIGN_TOP);
	Zelement_set_margin(track, Zthickness(0, 24 + VOICE_RING / 2 - 1, 0, 0));
	const Zid rest = border(Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
	Zelement_set_opacity(rest, 0.3f);
	Zpanel_add(track, rest);
	const Zid done = border(Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
	Zelement_set_horizontal_alignment(done, Z_HALIGN_LEFT);
	Zelement_set_width(done, 0);
	Zpanel_add(track, done);
	r.bars.push_back(done);
	Zgrid_place(g, track, c_track, 0);
	/* the time and the ticks under the line's end */
	const Zid meta = meta_line(r, m);
	Zelement_set_vertical_alignment(meta, Z_VALIGN_BOTTOM);
	Zelement_set_margin(meta, Zthickness(0, 0, 0, -3.0f));
	Zgrid_place(g, meta, c_track, 0);
	/* the seek: a press or a drag along the line */
	const Zid seek = Zthumb_create();
	Zcontrol_set_background(seek, Zcolor_rgba(0, 0, 0, 0));
	Zelement_set_height(seek, 30);
	Zelement_set_vertical_alignment(seek, Z_VALIGN_TOP);
	Zelement_set_margin(seek, Zthickness(0, 27, 0, 0));
	Zelement_set_name(seek, id.c_str());
	Zthumb_on_drag_started(seek, [](const Zid t, const float x, float, void*) {
		const std::string mid = Zelement_get_name(t);
		const int i = index_of(mid);
		if (i < 0) return;
		const double total = std::max(1, cv.msgs[size_t(i)].media.seconds);
		const double at = total * std::clamp(double(x) / std::max(1.0f, Zelement_get_actual_width(t)), 0.0, 1.0);
		cv.seek_id = mid;
		cv.voice_pos[mid] = at;
		Zelement_set_tag(t, reinterpret_cast<void*>(intptr_t(x)));
		voice_show(cv.rows[size_t(i)], cv.msgs[size_t(i)], at, total, voice::playing(mid));
	}, nullptr);
	Zthumb_on_drag_delta(seek, [](const Zid t, const float dx, float, void*) {
		const std::string mid = Zelement_get_name(t);
		const int i = index_of(mid);
		if (i < 0) return;
		const float x = float(reinterpret_cast<intptr_t>(Zelement_get_tag(t))) + dx;
		Zelement_set_tag(t, reinterpret_cast<void*>(intptr_t(x)));
		const double total = std::max(1, cv.msgs[size_t(i)].media.seconds);
		const double at = total * std::clamp(double(x) / std::max(1.0f, Zelement_get_actual_width(t)), 0.0, 1.0);
		cv.voice_pos[mid] = at;
		voice_show(cv.rows[size_t(i)], cv.msgs[size_t(i)], at, total, voice::playing(mid));
	}, nullptr);
	Zthumb_on_drag_completed(seek, [](const Zid t, float, float, void*) {
		const std::string mid = Zelement_get_name(t);
		cv.seek_id.clear();
		if (voice::playing(mid)) voice_play(mid, cv.voice_pos[mid]);
	}, nullptr);
	Zgrid_place(g, seek, c_track, 0);
	/* the mic beside the picture, at its foot */
	r.voice_mic = Zcontent_control_create();
	const Zid mic = symbol(Z_SYMBOL_MICROPHONE);
	Zelement_set_width(mic, 13);
	Zelement_set_height(mic, 13);
	Zcontent_control_set_content(r.voice_mic, mic);
	Zelement_set_vertical_alignment(r.voice_mic, Z_VALIGN_BOTTOM);
	Zelement_set_margin(r.voice_mic, b_out ? Zthickness(-14, 0, 0, 1) : Zthickness(1, 0, 3, 1));
	Zgrid_place(g, r.voice_mic, b_out ? c_pic : c_mic, 0);
	if (b_out) Zelement_set_horizontal_alignment(r.voice_mic, Z_HALIGN_RIGHT);
	/* the speaker's picture, the length over its corner */
	const std::string who = b_out ? st.self_jid : (cv.b_group ? m.sender : cv.id);
	const Zid pic_cell = Zgrid_create();
	const Zid pic = avatar(who, b_out ? "me" : m.sender_name.empty() ? cv.name : m.sender_name, VOICE_PIC, false);
	Zpanel_add(pic_cell, pic);
	r.dur = text_px(mmss(m.media.seconds), 15.0f, Z_FONT_NORMAL, Z_RES_NONE);
	Ztext_block_set_foreground(r.dur, literal(0xFFFFFFFF));
	Zelement_set_horizontal_alignment(r.dur, Z_HALIGN_RIGHT);
	Zelement_set_vertical_alignment(r.dur, Z_VALIGN_BOTTOM);
	Zelement_set_margin(r.dur, Zthickness(0, 0, 4, 1));
	Zpanel_add(pic_cell, r.dur);
	Zelement_set_vertical_alignment(pic_cell, Z_VALIGN_TOP);
	Zgrid_place(g, pic_cell, c_pic, 0);
	const double pos = cv.voice_pos.count(id) ? cv.voice_pos[id] : 0;
	voice_show(r, m, pos, m.media.seconds, voice::playing(id));
	return g;
}

/* ---- documents: the type, the name, the size; a tap downloads (once) and opens with the desktop's program ---- */
std::string doc_sub_line(const Msg& m) {
	std::string ext;
	const size_t dot = m.media.file_name.rfind('.');
	if (dot != std::string::npos && m.media.file_name.size() - dot <= 6) ext = upper(m.media.file_name.substr(dot + 1));
	else if (m.media.mime.find('/') != std::string::npos) ext = upper(m.media.mime.substr(m.media.mime.find('/') + 1, 4));
	std::string s = ext.empty() ? "file" : ext;
	if (m.media.size > 0) s += "  " + human_size(m.media.size);
	return s;
}

void doc_open(const std::string& id) {
	const int i = index_of(id);
	if (i < 0) return;
	const Msg& m = cv.msgs[size_t(i)];
	if (!m.media.path.empty() && access(m.media.path.c_str(), R_OK) == 0) {
		open_external(m.media.path);
		return;
	}
	Ztext_block_set_text(cv.rows[size_t(i)].doc_sub, "downloading...");
	request("download_media", {{"chat", cv.id}, {"message_id", id}}, [id](const json* res, const char* err) {
		if (!alive()) return;
		const int k = index_of(id);
		if (k < 0) return;
		const std::string p = err ? "" : jstr(*res, "path");
		Msg& mm = cv.msgs[size_t(k)];
		if (p.empty()) {
			Ztext_block_set_text(cv.rows[size_t(k)].doc_sub, err && !strcmp(err, "not_connected") ? "offline, tap to retry" : "not downloaded, tap to retry");
			return;
		}
		mm.media.path = p;
		Ztext_block_set_text(cv.rows[size_t(k)].doc_sub, doc_sub_line(mm).c_str());
		open_external(p);
	});
}

Zid doc_part(Row& r, const Msg& m) {
	const Zid g = Zgrid_create();
	Zgrid_add_column(g, Z_GRID_AUTO, 0);
	Zgrid_add_column(g, Z_GRID_STAR, 1);
	Zelement_set_min_size(g, 200, 0);
	const Zid sheet = border(m.b_from_me ? Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR : Z_RES_PHONE_BASE_LOW_COLOR);
	Zelement_set_width(sheet, 38);
	Zelement_set_height(sheet, 47.5f);
	Zelement_set_margin(sheet, Zthickness(0, 0, G, 0));
	const Zid ic = icon_holder(path_icon(glyph::page), 20, m.b_from_me ? Z_RES_PHONE_ACCENT_BRUSH : Z_RES_PHONE_FOREGROUND_BRUSH);
	Zelement_set_horizontal_alignment(ic, Z_HALIGN_CENTER);
	Zelement_set_vertical_alignment(ic, Z_VALIGN_CENTER);
	Zborder_set_child(sheet, ic);
	Zgrid_place(g, sheet, 0, 0);
	const Zid col = Zstack_panel_create(Z_VERTICAL);
	Zelement_set_vertical_alignment(col, Z_VALIGN_CENTER);
	const Zid name = text(m.media.file_name.empty() ? "document" : m.media.file_name, Z_RES_TEXT_STYLE_LARGE_FONT_SIZE, Z_FONT_NORMAL, fg_of(m));
	Ztext_block_set_text_wrapping(name, Z_WRAP);
	Ztext_block_set_max_lines(name, 2);
	Zpanel_add(col, name);
	r.doc_sub = text(doc_sub_line(m), Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, meta_of(m));
	set_trim(r.doc_sub);
	Zpanel_add(col, r.doc_sub);
	Zgrid_place(g, col, 1, 0);
	const Zid tap = tappable(g);
	const std::string id = m.id;
	click(tap, [id] { doc_open(id); });
	return tap;
}

/* ---- stickers: no bubble; downloaded at once, the first frame shown ---- */
void sticker_load(const std::string& id, const Zid img, const Zid hint) {
	const int i = index_of(id);
	if (i < 0) return;
	auto show = [img, hint](const std::string& path) {
		image_file(path, 142, 142, false, [img, hint](PixP p) {
			image_set(img, p);
			if (Zelement_is_valid(hint)) Zelement_set_visibility(hint, p ? Z_COLLAPSED : Z_VISIBLE);
		});
	};
	if (!cv.msgs[size_t(i)].media.path.empty()) {
		show(cv.msgs[size_t(i)].media.path);
		return;
	}
	Ztext_block_set_text(hint, "sticker...");
	Zelement_set_visibility(hint, Z_VISIBLE);
	request("download_media", {{"chat", cv.id}, {"message_id", id}}, [id, show, hint](const json* res, const char* err) {
		const std::string p = err ? "" : jstr(*res, "path");
		if (!alive()) return;
		if (p.empty()) {
			if (Zelement_is_valid(hint)) Ztext_block_set_text(hint, "sticker\ntap to retry");
			return;
		}
		const int k = index_of(id);
		if (k >= 0) cv.msgs[size_t(k)].media.path = p;
		show(p);
	});
}

void row_spacing(const int i);
void message_menu(const std::string& id);
void start_reply(const std::string& id);

/* ---- swipe right to reply (as WhatsApp): the bubble follows the finger, the arrow fades in; past 57 it replies ---- */
const char* const REPLY_ARROW = "M0,8 L8,0 L8,16 Z M8,5 L20,5 L20,11 L8,11 Z"; /* convex runs: the painter fills polygons */
constexpr float SWIPE_REPLY = 57.0f, SWIPE_MAX = 85.0f;

/* the bubble's place in its row, moved by margins (a translate inside a list row is not repainted in every case) */
/* render translates, not margins: the bubble follows the finger without a layout of the list each frame (spec 7.15) */
void swipe_at(Row& r, const float x) {
	Zelement_set_translate(r.block, x, 0);
	Zelement_set_opacity(r.swipe_icon, std::min(1.0f, x / SWIPE_REPLY));
	Zelement_set_translate(r.swipe_icon, std::min(0.0f, x - SWIPE_REPLY) / 2.0f, 0);
}

void swipe_back(const std::string& id, const float from, const int step) {
	Row* r = alive() ? row_of(id) : nullptr;
	if (!r) return;
	const float t = float(step) / 12.0f, e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
	swipe_at(*r, from * (1.0f - e));
	if (step < 12) after(16, [id, from, step] { swipe_back(id, from, step + 1); });
	else {
		r->b_swiping = false;
		gap::cache_bitmap(r->container);
	}
}

void swipe_row(const std::string& id, const ZSwipePhase phase, const float dx) {
	if (!alive()) return;
	Row* r = row_of(id);
	if (!r || !Zelement_is_valid(r->block)) return;
	if (phase == Z_SWIPE_STARTED && !r->b_swiping) {
		r->b_swiping = true;
		gap::cache_none(r->container); /* the bubble moves inside the row */
	}
	/* only to the right; past the reply point it moves at a third of the finger's pace */
	float x = std::max(0.0f, dx);
	if (x > SWIPE_REPLY) x = SWIPE_REPLY + (x - SWIPE_REPLY) / 3.0f;
	x = std::min(x, SWIPE_MAX);
	if (phase != Z_SWIPE_COMPLETED) {
		swipe_at(*r, x);
		return;
	}
	swipe_back(id, x, 1);
	if (dx >= SWIPE_REPLY) start_reply(id);
}

/* One message as a row of the list. */
Row build_row(const int i) {
	const Msg& m = cv.msgs[size_t(i)];
	Row r;
	r.id = m.id;
	const Zid outer = Zstack_panel_create(Z_VERTICAL);
	r.date = text(day_label(m.ts), Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_SEMIBOLD, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
	Zelement_set_horizontal_alignment(r.date, Z_HALIGN_CENTER);
	Zelement_set_margin(r.date, Zthickness(0, M, 0, G));
	Zpanel_add(outer, r.date);
	if (m.kind == "system") {
		const Zid t = text(m.text, Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
		Ztext_block_set_text_wrapping(t, Z_WRAP);
		Ztext_block_set_text_alignment(t, Z_TEXT_CENTER);
		Zelement_set_max_size(t, 290, Z_INFINITE);
		Zelement_set_horizontal_alignment(t, Z_HALIGN_CENTER);
		Zelement_set_margin(t, Zthickness(0, H, 0, H));
		r.block = t;
		Zpanel_add(outer, t);
	} else if (m.kind == "sticker") {
		const Zid s = Zstack_panel_create(Z_VERTICAL);
		Zelement_set_horizontal_alignment(s, m.b_from_me ? Z_HALIGN_RIGHT : Z_HALIGN_LEFT);
		const Zid box = Zgrid_create();
		Zelement_set_width(box, 142);
		Zelement_set_height(box, 142);
		const Zid img = Zimage_create();
		Zimage_set_stretch(img, Z_STRETCH_UNIFORM);
		Zpanel_add(box, img);
		/* until it is here (or when it failed): a quiet placeholder that retries on a tap */
		const Zid hint = text("sticker", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
		Ztext_block_set_text_alignment(hint, Z_TEXT_CENTER);
		Zelement_set_horizontal_alignment(hint, Z_HALIGN_CENTER);
		Zelement_set_vertical_alignment(hint, Z_VALIGN_CENTER);
		Zelement_set_visibility(hint, Z_COLLAPSED);
		Zpanel_add(box, hint);
		const Zid tap = tappable(box);
		const std::string sid = m.id;
		click(tap, [sid, img, hint] {
			const int k = index_of(sid);
			if (k >= 0 && cv.msgs[size_t(k)].media.path.empty()) sticker_load(sid, img, hint);
		});
		Zpanel_add(s, tap);
		const Zid chip = border(Z_RES_CONTENT_DIALOG_DIMMING_COLOR);
		Zborder_set_padding(chip, Zthickness(H, 0, H, 0));
		Zelement_set_horizontal_alignment(chip, m.b_from_me ? Z_HALIGN_RIGHT : Z_HALIGN_LEFT);
		Zborder_set_child(chip, meta_line(r, m, true));
		Zpanel_remove(Zelement_get_parent(r.edited), r.edited);
		Zelement_destroy(r.edited);
		r.edited = Z_INVALID_ID;
		Zpanel_add(s, chip);
		if (!m.media.path.empty() || st.prefs.b_stickers) {
			const std::string id = m.id;
			/* after the row is in the list: the load finds it by id */
			post([id, img, hint] {
				if (alive() && Zelement_is_valid(img)) sticker_load(id, img, hint);
			});
		} else {
			Zelement_set_visibility(hint, Z_VISIBLE);
			Ztext_block_set_text(hint, "sticker\ntap to show");
		}
		r.block = s;
		Zelement_set_margin(s, Zthickness(0, H, 0, 0));
		Zpanel_add(outer, s);
	} else {
		const Zid block = Zstack_panel_create(Z_VERTICAL);
		Zelement_set_horizontal_alignment(block, m.b_from_me ? Z_HALIGN_RIGHT : Z_HALIGN_LEFT);
		Zelement_set_margin(block, m.b_from_me ? Zthickness(BUBBLE_AWAY, 0, 0, 0) : Zthickness(0, 0, BUBBLE_AWAY, 0));
		const Zid b = border(Z_RES_PHONE_ACCENT_BRUSH);
		/* every bubble of the phone app is the full width, whatever it holds (reference: a one-line "K." as wide as a
		 * paragraph; incoming x 18-290, outgoing x 70-340); a picture's bubble is the picture's width below */
		Zelement_set_width(b, BUBBLE_MAX);
		Zelement_set_horizontal_alignment(b, m.b_from_me ? Z_HALIGN_RIGHT : Z_HALIGN_LEFT);
		/* the shade of an outgoing bubble fills it, padding and all; the content sits in it at the bubble's padding */
		const Zid shade = Zgrid_create();
		Zpanel_set_background(shade, m.b_from_me ? Zcolor_rgba(0, 0, 0, OUT_SHADE) : Zcolor_rgba(0, 0, 0, 0));
		Zborder_set_child(b, shade);
		const Zid col = Zstack_panel_create(Z_VERTICAL);
		Zelement_set_margin(col, Zthickness(9.5f, 6.0f, 10.0f, 6.4f));
		Zpanel_add(shade, col);
		if (cv.b_group && !m.b_from_me) {
			r.sender = text_px(m.sender_name, SENDER_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
			Zelement_set_opacity(r.sender, 0.6f);
			Zelement_set_margin(r.sender, Zthickness(0, -3.5f, 0, 0));
			set_trim(r.sender);
			Zpanel_add(col, r.sender);
		}
		if (!m.quoted_id.empty() && !m.b_deleted) Zpanel_add(col, reply_quote(m));
		const bool b_image = m.kind == "image" && m.b_media;
		if (b_image) {
			Zelement_set_margin(col, Zthickness(H, H, H, H));
			Zelement_set_width(b, Z_AUTO);
			Zelement_set_max_size(b, BUBBLE_MAX - 9.5f, Z_INFINITE);
			Zpanel_add(col, image_part(r, m, m.text.empty()));
		}
		if (m.b_deleted) {
			r.body = text_px("this message was deleted", BODY_SIZE, Z_FONT_NORMAL, fg_of(m));
			Zelement_set_opacity(r.body, 0.6f);
			Zpanel_add(col, r.body);
		} else if (m.kind == "text" || m.kind == "unsupported" || m.kind == "undecryptable" || m.kind == "contact" || m.kind == "location" ||
			(b_image && !m.text.empty())) {
			int n = 0;
			const bool b_big = m.kind == "text" && emoji_only(m.text, &n);
			std::string body = m.text;
			if (body.empty()) body = m.kind;
			r.body = b_big ? text(body, Z_RES_TEXT_STYLE_EXTRA_EXTRA_LARGE_FONT_SIZE, Z_FONT_NORMAL, fg_of(m))
				: text_px(body, BODY_SIZE, Z_FONT_NORMAL, fg_of(m));
			Ztext_block_set_text_wrapping(r.body, Z_WRAP);
			Ztext_block_set_line_height(r.body, b_big ? 46 : BODY_LINE);
			if (b_image) Zelement_set_margin(r.body, Zthickness(H, H, H, 0));
			Zpanel_add(col, r.body);
		} else if (m.kind == "audio") {
			Zpanel_add(col, voice_part(r, m));
		} else if (m.kind == "document") {
			Zpanel_add(col, doc_part(r, m));
			if (!m.text.empty()) {
				r.body = text_px(m.text, BODY_SIZE, Z_FONT_NORMAL, fg_of(m));
				Ztext_block_set_text_wrapping(r.body, Z_WRAP);
				Ztext_block_set_line_height(r.body, BODY_LINE);
				Zelement_set_margin(r.body, Zthickness(0, H, 0, 0));
				Zpanel_add(col, r.body);
			}
		} else if (!b_image) {
			Zpanel_add(col, kind_row(m));
		}
		/* a voice note carries its time inside (under the line, before the picture) */
		if (!(b_image && m.text.empty()) && m.kind != "audio") {
			const Zid meta = meta_line(r, m);
			Zelement_set_margin(meta, Zthickness(0, 2.0f, 0, 0));
			Zpanel_add(col, meta);
		}
		if (m.kind == "audio") Zelement_set_margin(col, Zthickness(7.0f, 7.0f, 7.0f, 6.0f));
		/* the tails are the bubble's own: in its grid, placed from its edges (hanging out over them by the tail's height, a
		 * pixel into the bubble), so they are laid out, cached and turned with it and never drawn apart from it (an outgoing
		 * tail showed a gap under its bubble on the first frames of a chat). row_spacing gives the block the room. */
		const Zid bub = Zgrid_create();
		Zelement_set_horizontal_alignment(bub, m.b_from_me ? Z_HALIGN_RIGHT : Z_HALIGN_LEFT);
		Zpanel_add(bub, b);
		r.tail_top = tail_icon(TAIL_IN, false);
		Zelement_set_horizontal_alignment(r.tail_top, Z_HALIGN_LEFT);
		Zelement_set_vertical_alignment(r.tail_top, Z_VALIGN_TOP);
		Zelement_set_margin(r.tail_top, Zthickness(TAIL_IN_X, 1.0f - TAIL_H, 0, 0));
		Zelement_set_visibility(r.tail_top, Z_COLLAPSED);
		Zpanel_add(bub, r.tail_top);
		r.tail_bottom = tail_icon(TAIL_OUT, true);
		Zelement_set_horizontal_alignment(r.tail_bottom, Z_HALIGN_RIGHT);
		Zelement_set_vertical_alignment(r.tail_bottom, Z_VALIGN_BOTTOM);
		Zelement_set_margin(r.tail_bottom, Zthickness(0, 0, TAIL_OUT_X, 1.0f - TAIL_H));
		Zelement_set_visibility(r.tail_bottom, Z_COLLAPSED);
		Zpanel_add(bub, r.tail_bottom);
		Zpanel_add(block, bub);
		if (!m.reactions.empty()) {
			/* below the bubble, overlapping it by 9.5, at the outer edge (spec 3.3) */
			const Zid chip = border(Z_RES_PHONE_CHROME_COLOR);
			Zborder_bind_border_brush(chip, Z_RES_PHONE_BACKGROUND_BRUSH);
			Zborder_set_border_thickness(chip, Zthickness_uniform(2));
			Zborder_set_padding(chip, Zthickness_uniform(H));
			Zelement_set_margin(chip, Zthickness(0, -9.5f, 0, 0));
			Zelement_set_horizontal_alignment(chip, m.b_from_me ? Z_HALIGN_LEFT : Z_HALIGN_RIGHT);
			std::string s;
			for (const Reaction& re : m.reactions) s += re.emoji + (re.count > 1 ? " " + std::to_string(re.count) : "") + " ";
			Zborder_set_child(chip, text(s, Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_FOREGROUND_BRUSH));
			Zpanel_add(block, chip);
		}
		r.block = block;
		Zpanel_add(outer, block);
	}
	/* the reply arrow that a swipe to the right pulls out from under the bubble */
	const Zid cell = Zgrid_create();
	Zpanel_add(cell, outer);
	if (m.kind != "system" && !m.b_deleted && !cv.b_channel) {
		r.swipe_icon = icon_holder(path_icon(REPLY_ARROW), 20, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
		Zelement_set_horizontal_alignment(r.swipe_icon, Z_HALIGN_LEFT);
		Zelement_set_vertical_alignment(r.swipe_icon, Z_VALIGN_CENTER);
		Zelement_set_opacity(r.swipe_icon, 0);
		Zpanel_add(cell, r.swipe_icon);
	}
	r.container = Zlist_view_item_create(nullptr);
	Zcontent_control_set_content(r.container, cell);
	Zcontrol_set_horizontal_content_alignment(r.container, Z_HALIGN_STRETCH);
	Zelement_set_tilt(r.container, false); /* no tilt on bubbles (spec 4.4) */
	Zelement_set_name(r.container, m.id.c_str());
	gap::cache_bitmap(r.container);
	gap::on_holding(r.container, [](const Zid sender, void*) { message_menu(Zelement_get_name(sender)); }, nullptr);
	if (Zelement_is_valid(r.swipe_icon)) gap::on_swipe(r.container, [](const Zid sender, const ZSwipePhase phase, const float dx, void*) { swipe_row(Zelement_get_name(sender), phase, dx); });
	return r;
}

/* the date line, the tail and the gap of row i follow from its neighbours (spec 3.1, 3.4) */
void row_spacing(const int i) {
	if (i < 0 || size_t(i) >= cv.rows.size()) return;
	Row& r = cv.rows[size_t(i)];
	const Msg& m = cv.msgs[size_t(i)];
	const Msg* prev = i > 0 ? &cv.msgs[size_t(i) - 1] : nullptr;
	const Msg* next = size_t(i) + 1 < cv.msgs.size() ? &cv.msgs[size_t(i) + 1] : nullptr;
	const bool b_date = prev ? !same_day(prev->ts, m.ts) : !cv.b_more;
	Zelement_set_visibility(r.date, b_date ? Z_VISIBLE : Z_COLLAPSED);
	if (!bubbled(m)) return;
	const bool b_with_prev = prev && grouped(*prev, m) && !b_date;
	const bool b_with_next = next && grouped(m, *next);
	const bool b_top = !m.b_from_me && !b_with_prev;
	const bool b_bottom = m.b_from_me && !b_with_next;
	Zelement_set_visibility(r.tail_top, b_top ? Z_VISIBLE : Z_COLLAPSED);
	Zelement_set_visibility(r.tail_bottom, b_bottom ? Z_VISIBLE : Z_COLLAPSED);
	/* 4.75 inside a group; between groups 15 bubble to bubble, 5 between tails (the phone app) */
	const float top = b_with_prev ? H : (b_top ? 5.0f : 15.0f - (prev && prev->b_from_me && !grouped(*prev, m) && bubbled(*prev) ? TAIL_H : 0.0f));
	ZThickness mg = Zelement_get_margin(r.block);
	/* the room of the tails, which hang out of the bubble (a pixel of each is inside it) */
	mg.top = top + (b_top ? TAIL_H - 1.0f : 0.0f);
	mg.bottom = b_bottom ? TAIL_H - 1.0f : 0.0f;
	Zelement_set_margin(r.block, mg);
	if (Zelement_is_valid(r.sender)) Zelement_set_visibility(r.sender, b_with_prev ? Z_COLLAPSED : Z_VISIBLE);
}

double end_offset() { return std::max(0.0, Zscroll_viewer_get_extent_height(cv.sv) - Zscroll_viewer_get_viewport_height(cv.sv)); }
bool at_end() { return Zscroll_viewer_get_vertical_offset(cv.sv) >= end_offset() - 2; }

void jump_update() {
	if (!alive()) return;
	const double from_end = end_offset() - Zscroll_viewer_get_vertical_offset(cv.sv);
	const bool b_show = from_end > Zscroll_viewer_get_viewport_height(cv.sv);
	Zelement_set_visibility(cv.jump, b_show ? Z_VISIBLE : Z_COLLAPSED);
	if (!b_show && cv.unseen) cv.unseen = 0;
	Zelement_set_visibility(cv.badge, cv.unseen ? Z_VISIBLE : Z_COLLAPSED);
	Ztext_block_set_text(cv.badge_text, std::to_string(cv.unseen).c_str());
}

void scroll_center(const int index) {
	Zroot_update_layout(g_root);
	const Zid c = cv.rows[size_t(index)].container;
	const ZRect b = Zelement_get_bounds(c);
	Zscroll_viewer_scroll_to_vertical_offset(cv.sv, b.y + b.height / 2 - Zscroll_viewer_get_viewport_height(cv.sv) / 2);
	row_flash(c);
}

void mark_read() {
	auto it = st.chats.find(cv.id);
	if (!cv.b_channel && it != st.chats.end() && it->second.unread) request("mark_read", {{"chat", cv.id}});
}

void header_status() {
	if (!alive()) return;
	const bool b_typing = st.typing.count(cv.id) && st.typing[cv.id];
	std::string line = b_typing ? "typing..." : st.presence.count(cv.id) ? st.presence[cv.id] : "";
	if (cv.b_group && !b_typing) line = cv.members;
	if (cv.b_channel) line = "channel";
	Ztext_block_set_text(cv.status, line.c_str());
	Ztext_block_bind_foreground(cv.status, b_typing ? Z_RES_PHONE_ACCENT_BRUSH : Z_RES_PHONE_BASE_MID_COLOR);
}

/* ---- the typing indicator (spec 3.4): three squares pulsing 0.3-1.0, 1200 ms, 200 ms apart ---- */
void typing_pulse(std::vector<Zid> dots, const bool b_up) {
	if (!alive() || !Zelement_is_valid(cv.typing)) return;
	for (size_t k = 0; k < dots.size(); k++) Zelement_animate(dots[k], Z_PROP_OPACITY, b_up ? 1.0f : 0.3f, 600, Z_EASE_LINEAR, 200.0f * float(k));
	cv.typing_timer = after(600, [dots, b_up] { typing_pulse(dots, !b_up); });
}

void typing_show(const bool b_on) {
	if (!alive() || !cv.b_loaded) return;
	if (b_on == Zelement_is_valid(cv.typing)) return;
	if (!b_on) {
		cancel(cv.typing_timer);
		const Zid t = cv.typing;
		cv.typing = Z_INVALID_ID;
		Zelement_animate(t, Z_PROP_OPACITY, 0, 167, Z_EASE_OUT, 0);
		after(170, [t] {
			if (!Zelement_is_valid(t)) return;
			const bool b_end = alive() && at_end();
			Zpanel_remove(Zelement_get_parent(t), t);
			Zelement_destroy(t);
			if (b_end) {
				Zroot_update_layout(g_root);
				Zscroll_viewer_scroll_to_vertical_offset(cv.sv, end_offset());
			}
		});
		return;
	}
	const Zid block = Zstack_panel_create(Z_VERTICAL);
	Zelement_set_horizontal_alignment(block, Z_HALIGN_LEFT);
	Zelement_set_margin(block, Zthickness(0, 14.25f - 9.5f, 0, 0));
	const Zid tail = tail_icon(TAIL_IN, false);
	Zelement_set_horizontal_alignment(tail, Z_HALIGN_LEFT);
	Zelement_set_margin(tail, Zthickness(TAIL_IN_X, 0, 0, -1));
	Zpanel_add(block, tail);
	const Zid b = border(Z_RES_PHONE_ACCENT_BRUSH);
	Zborder_set_padding(b, Zthickness(G, G, G, G));
	Zelement_set_horizontal_alignment(b, Z_HALIGN_LEFT);
	const Zid dots = Zstack_panel_create(Z_HORIZONTAL);
	std::vector<Zid> v;
	for (int k = 0; k < 3; k++) {
		const Zid d = border(Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
		Zelement_set_width(d, 6.5f);
		Zelement_set_height(d, 6.5f);
		Zelement_set_margin(d, Zthickness(k ? H : 0, 0, 0, 0));
		Zelement_set_opacity(d, 0.3f);
		Zpanel_add(dots, d);
		v.push_back(d);
	}
	Zborder_set_child(b, dots);
	Zpanel_add(block, b);
	const bool b_end = at_end();
	cv.typing = Zlist_view_item_create(nullptr);
	Zcontent_control_set_content(cv.typing, block);
	Zcontrol_set_horizontal_content_alignment(cv.typing, Z_HALIGN_STRETCH);
	Zelement_set_tilt(cv.typing, false);
	Zitems_control_add(cv.list, cv.typing);
	if (b_end) {
		Zroot_update_layout(g_root);
		Zscroll_viewer_scroll_to_vertical_offset(cv.sv, end_offset());
	}
	typing_pulse(v, true);
}

/* Spec 4.6-4.8: a row at the end; the list follows only when it was at the end (or the row is ours). */
void append_row(const bool b_animate, const bool b_mine) {
	const int i = int(cv.msgs.size()) - 1;
	cv.rows.push_back(build_row(i));
	Row& r = cv.rows.back();
	const bool b_end = at_end() || b_mine;
	const Zid typing = cv.typing;
	if (Zelement_is_valid(typing)) Zpanel_remove(cv.panel, typing);
	Zitems_control_add(cv.list, r.container);
	if (Zelement_is_valid(typing)) Zpanel_add(cv.panel, typing);
	row_spacing(i);
	row_spacing(i - 1);
	gap::anchor(cv.sv, r.container);
	if (b_end && Zcontent_control_get_content(frame()) != cv.page) {
		/* not laid out while it is off the frame: an offset set now would be from the old sizes */
		cv.b_end_pending = true;
		return;
	}
	if (!b_end) {
		cv.unseen++;
		jump_update();
		Zelement_set_render_scale(cv.badge, 1.3f);
		Zelement_animate(cv.badge, Z_PROP_SCALE, 1, 167, Z_EASE_OUT, 0);
		return;
	}
	const double before = Zscroll_viewer_get_vertical_offset(cv.sv);
	Zroot_update_layout(g_root);
	const double to = end_offset();
	Zscroll_viewer_scroll_to_vertical_offset(cv.sv, to);
	if (!b_animate) return;
	/* the scroll as a render translate of the panel: no relayout per frame */
	const float moved = float(to - before);
	if (moved > 0) {
		Zelement_set_translate(cv.panel, 0, moved);
		Zelement_animate(cv.panel, Z_PROP_TRANSLATE_Y, 0, 250, Z_EASE_OUT, 0);
	}
	const float rise = b_mine ? 47.5f : 28.5f;
	Zelement_set_translate(r.container, 0, rise);
	Zelement_set_opacity(r.container, 0);
	Zelement_animate(r.container, Z_PROP_TRANSLATE_Y, 0, b_mine ? 200 : 250, b_mine ? Z_EASE_EMPHASIZED : Z_EASE_OUT, 0);
	Zelement_animate(r.container, Z_PROP_OPACITY, 1, b_mine ? 200 : 250, b_mine ? Z_EASE_EMPHASIZED : Z_EASE_OUT, 0);
}

void land();

/* the first page: rows, the unread divider, and the scroll set before the first painted frame (spec 7) */
void fill_first(const json& list, const int unread) {
	for (auto& o : list) {
		Msg m = msg_from(o);
		if (m.kind == "reaction" || m.kind == "hidden" || m.id.empty()) continue;
		if (m.chat.empty()) m.chat = cv.id;
		cv.msgs.push_back(std::move(m));
	}
	int divider_at = -1;
	if (unread > 0) {
		int n = 0;
		for (int i = int(cv.msgs.size()) - 1; i >= 0; i--)
			if (!cv.msgs[size_t(i)].b_from_me && ++n == unread) {
				divider_at = i;
				break;
			}
	}
	for (int i = 0; i < int(cv.msgs.size()); i++) {
		if (i == divider_at) {
			const Zid band = border(Z_RES_PHONE_BASE_LOW_COLOR);
			Zelement_set_height(band, 28.5f);
			Zelement_set_margin(band, Zthickness(-G, H, -G, 0));
			const Zid t = text(std::to_string(unread) + (unread == 1 ? " unread message" : " unread messages"),
				Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_SEMIBOLD, Z_RES_PHONE_ACCENT_BRUSH);
			Zelement_set_horizontal_alignment(t, Z_HALIGN_CENTER);
			Zelement_set_vertical_alignment(t, Z_VALIGN_CENTER);
			Zborder_set_child(band, t);
			cv.divider = Zlist_view_item_create(nullptr);
			Zcontent_control_set_content(cv.divider, band);
			Zcontrol_set_horizontal_content_alignment(cv.divider, Z_HALIGN_STRETCH);
			Zelement_set_tilt(cv.divider, false);
			Zelement_set_name(cv.divider, "");
			Zitems_control_add(cv.list, cv.divider);
		}
		cv.rows.push_back(build_row(i));
		Zitems_control_add(cv.list, cv.rows.back().container);
	}
	for (int i = 0; i < int(cv.rows.size()); i++) row_spacing(i);
	/* G9: the last message keeps its place when older pages, the typing row or the keyboard change the list */
	if (!cv.rows.empty()) gap::anchor(cv.sv, cv.rows.back().container);
	cv.b_land_pending = true;
	land();
}

/* Spec 7: the chat opens at the unread divider or the bottom. Only once the list is laid out in the page: a page still held
 * back by the turnstile has no viewport, and an offset set then leaves the newest messages under the composer. */
void land() {
	if (!alive() || !cv.b_land_pending) return;
	Zroot_update_layout(g_root);
	if (Zscroll_viewer_get_viewport_height(cv.sv) <= 0) return;
	cv.b_land_pending = false;
	if (Zelement_is_valid(cv.divider)) {
		const ZRect b = Zelement_get_bounds(cv.divider);
		Zscroll_viewer_scroll_to_vertical_offset(cv.sv, std::min(end_offset(), double(b.y) - Zscroll_viewer_get_viewport_height(cv.sv) / 3));
	} else {
		Zscroll_viewer_scroll_to_vertical_offset(cv.sv, end_offset());
	}
	jump_update();
}

/* Spec 3.4 / 7: older messages go in above; the first message on screen stays where it is, to the pixel. */
void load_older() {
	if (cv.b_older_pending || !cv.b_more || cv.msgs.empty() || cv.b_channel) return;
	cv.b_older_pending = true;
	Zelement_set_visibility(cv.older, Z_VISIBLE);
	const std::string id = cv.id;
	request("messages", {{"chat", id}, {"limit", PAGE_SIZE}, {"before_ts", cv.msgs.front().ts}, {"before_id", cv.msgs.front().id}},
		[id](const json* r, const char* err) {
			if (!alive() || id != cv.id) return;
			cv.b_older_pending = false;
			Zelement_set_visibility(cv.older, Z_COLLAPSED);
			if (err || !r->contains("messages")) return;
			std::vector<Msg> got;
			for (auto& o : (*r)["messages"]) {
				Msg m = msg_from(o);
				if (m.kind == "reaction" || m.kind == "hidden" || m.id.empty() || index_of(m.id) >= 0) continue;
				if (m.chat.empty()) m.chat = cv.id;
				got.push_back(std::move(m));
			}
			cv.b_more = jbool(*r, "more");
			const double off = Zscroll_viewer_get_vertical_offset(cv.sv);
			const double ext = Zscroll_viewer_get_extent_height(cv.sv);
			for (int k = int(got.size()) - 1; k >= 0; k--) {
				cv.msgs.push_front(std::move(got[size_t(k)]));
				cv.rows.push_front(build_row(0));
				Zpanel_insert(cv.panel, 0, cv.rows.front().container);
			}
			for (int i = 0; i <= int(got.size()) && i < int(cv.rows.size()); i++) row_spacing(i);
			if (!gap::have_anchor()) {
				Zroot_update_layout(g_root);
				Zscroll_viewer_scroll_to_vertical_offset(cv.sv, off + (Zscroll_viewer_get_extent_height(cv.sv) - ext));
			}
		});
}

void strip_set(const std::string& label, const Msg* m) {
	if (!m) {
		cv.reply_id.clear();
		cv.edit_id.clear();
		Zelement_set_visibility(cv.strip, Z_COLLAPSED);
		return;
	}
	Ztext_block_set_text(cv.strip_label, label.c_str());
	Ztext_block_set_text(cv.strip_quote, label_of(*m).c_str());
	Zelement_set_visibility(cv.strip, Z_VISIBLE);
	if (!cv.b_pane) Zelement_focus(cv.box);
}

void start_reply(const std::string& id) {
	const Msg* q = find_msg(id);
	if (!q || cv.b_channel) return;
	cv.reply_id = id;
	cv.edit_id.clear();
	strip_set(q->b_from_me ? "you" : q->sender_name, q);
}

void send_enable() {
	if (!alive() || !Zelement_is_valid(cv.send)) return;
	const char* t = Ztext_box_get_text(cv.box);
	bool b_text = false;
	for (const char* p = t; *p; p++)
		if (!isspace(static_cast<unsigned char>(*p))) b_text = true;
	Zelement_set_enabled(cv.send, b_text && engine_up());
	Zelement_set_tag(cv.send, reinterpret_cast<void*>(1));
}

void do_send() {
	std::string text = Ztext_box_get_text(cv.box);
	while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
	if (text.empty() || !engine_up()) return;
	Ztext_box_set_text(cv.box, "");
	cv.last_text.clear();
	request("typing", {{"chat", cv.id}, {"state", "paused"}});
	cv.last_typing = 0;
	if (!cv.edit_id.empty()) {
		const std::string id = cv.edit_id;
		strip_set("", nullptr);
		request("edit_message", {{"chat", cv.id}, {"id", id}, {"text", text}}, [id, text](const json*, const char* err) {
			if (err || !alive()) return;
			const int i = index_of(id);
			if (i < 0) return;
			cv.msgs[size_t(i)].text = text;
			cv.msgs[size_t(i)].b_edited = true;
			Row& r = cv.rows[size_t(i)];
			if (Zelement_is_valid(r.body)) Ztext_block_set_text(r.body, text.c_str());
			if (Zelement_is_valid(r.edited)) Zelement_set_visibility(r.edited, Z_VISIBLE);
		});
		return;
	}
	/* optimistic (spec 4.8): the bubble with its clock now; the engine's id replaces the local one */
	Msg m;
	m.id = "local-" + std::to_string(++cv.local);
	m.chat = cv.id;
	m.b_from_me = true;
	m.sender = st.self_jid;
	m.kind = "text";
	m.text = text;
	m.status = "pending";
	m.quoted_id = cv.reply_id;
	m.ts = time(nullptr);
	const std::string local = m.id;
	json req = {{"chat", cv.id}, {"text", text}};
	if (!cv.reply_id.empty()) req["quoted_id"] = cv.reply_id;
	strip_set("", nullptr);
	cv.msgs.push_back(std::move(m));
	append_row(true, true);
	request("send_text", req, [local](const json* r, const char* err) {
		if (!alive()) return;
		const int i = index_of(local);
		if (i < 0) return;
		Msg& mm = cv.msgs[size_t(i)];
		if (err) {
			mm.status = "failed";
		} else {
			const std::string real = jstr(*r, "id");
			const int dup = index_of(real);
			if (!real.empty() && dup < 0) {
				mm.id = real;
				cv.rows[size_t(i)].id = real;
				Zelement_set_name(cv.rows[size_t(i)].container, real.c_str());
			}
		}
		tick_set(cv.rows[size_t(i)], mm);
	});
}

void delete_row(const int i) {
	Zitems_control_remove_at(cv.list, Zitems_control_index_from_container(cv.list, cv.rows[size_t(i)].container));
	cv.rows.erase(cv.rows.begin() + i);
	cv.msgs.erase(cv.msgs.begin() + i);
	row_spacing(i - 1);
	row_spacing(i);
}

void save_file(const std::string& from, const std::string& to) {
	work([from, to] {
		FILE* a = fopen(from.c_str(), "rb");
		FILE* b = a ? fopen(to.c_str(), "wb") : nullptr;
		char buf[65536];
		size_t n;
		while (a && b && (n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
		if (a) fclose(a);
		if (b) fclose(b);
	});
}

/* Spec 2.2: the message's menu (Holding with G2, a tap until then) */
void message_menu(const std::string& id) {
	const int i = index_of(id);
	if (i < 0 || cv.b_channel) return;
	const Msg m = cv.msgs[size_t(i)];
	const Zid menu = Zmenu_flyout_create();
	if (m.status == "failed") {
		menu_item(menu, "retry", [id] {
			const int k = index_of(id);
			if (k < 0) return;
			const Msg f = cv.msgs[size_t(k)];
			if (f.kind != "text" && !f.media.path.empty()) {
				/* a file that did not go: again from the engine's own copy */
				json req = {{"chat", cv.id}, {"kind", f.kind}, {"path", f.media.path}, {"mime", f.media.mime}, {"width", f.media.width},
					{"height", f.media.height}};
				if (f.kind == "audio") {
					req["ptt"] = f.media.b_ptt;
					req["seconds"] = f.media.seconds;
					req["waveform"] = f.media.waveform;
				}
				if (!f.media.file_name.empty()) req["file_name"] = f.media.file_name;
				if (!f.quoted_id.empty()) req["quoted_id"] = f.quoted_id;
				request("delete_message", {{"chat", cv.id}, {"id", id}});
				delete_row(k);
				request("send_media", req);
				return;
			}
			const std::string t = cv.msgs[size_t(k)].text;
			delete_row(k);
			Ztext_box_set_text(cv.box, t.c_str());
			do_send();
		});
		menu_item(menu, "edit", [id] {
			const int k = index_of(id);
			if (k < 0) return;
			const std::string t = cv.msgs[size_t(k)].text;
			delete_row(k);
			Ztext_box_set_text(cv.box, t.c_str());
			Zelement_focus(cv.box);
		});
		menu_item(menu, "delete", [id] {
			const int k = index_of(id);
			if (k >= 0) delete_row(k);
		});
		Zflyout_base_show_at(menu, cv.rows[size_t(i)].container);
		return;
	}
	if (m.kind == "system" || m.b_deleted) {
		Zelement_destroy(menu);
		return;
	}
	menu_item(menu, "reply", [id] { start_reply(id); });
	if (m.b_from_me && m.kind == "text" && time(nullptr) - m.ts < 15 * 60) {
		menu_item(menu, "edit", [id] {
			cv.edit_id = id;
			cv.reply_id.clear();
			const Msg* q = find_msg(id);
			if (q) Ztext_box_set_text(cv.box, q->text.c_str());
			strip_set("editing", q);
		});
	}
	menu_item(menu, "delete for me", [id] {
		confirm("delete?", "The message is deleted from this phone.", "delete", [id] {
			request("delete_message", {{"chat", cv.id}, {"id", id}}, [id](const json*, const char* err) {
				if (err || !alive()) return;
				const int k = index_of(id);
				if (k >= 0) delete_row(k);
			});
		});
	});
	if (m.b_from_me) {
		menu_item(menu, "delete for everyone", [id] {
			confirm("delete for everyone?", "The message is deleted for everyone in this chat.", "delete", [id] {
				request("delete_message", {{"chat", cv.id}, {"id", id}, {"for_everyone", true}}, [](const json*, const char* err) {
					if (err) confirm("not deleted", "Are you online? Try again later.", "ok", [] {});
				});
			});
		});
	}
	if (m.kind == "sticker" && !m.media.path.empty())
		menu_item(menu, "save sticker", [m] { save_file(m.media.path, stickers_dir() + "/" + m.id + ".webp"); });
	if (m.kind == "image" && !m.media.path.empty()) {
		menu_item(menu, "save", [m] {
			const char* home = getenv("HOME");
			const std::string dir = std::string(home ? home : "") + "/Pictures";
			save_file(m.media.path, dir + "/whatsapp-" + m.id + ".jpg");
		});
	}
	Zflyout_base_show_at(menu, cv.rows[size_t(i)].container);
}

/* ---- S14: the emoji and stickers panel. It takes the keyboard's place (WhatsApp): the keyboard goes, the panel is
 * the same height where it was, and the composer does not move; a tap on the text box brings the keyboard back. ---- */
float g_kb_h = 260.0f; /* the keyboard's last height, from the input pane (G5) */
bool g_tab_stickers = false;

const char* const EMOJI[] = {"😀", "😁", "😂", "🤣", "😃", "😄", "😅", "😆", "😉", "😊", "😋", "😎", "😍", "😘", "🥰", "😗",
	"🙂", "🤗", "🤩", "🤔", "🤨", "😐", "😑", "😶", "🙄", "😏", "😣", "😥", "😮", "🤐", "😯", "😪", "😫", "🥱", "😴", "😌",
	"😛", "😜", "😝", "🤤", "😒", "😓", "😔", "😕", "🙃", "🤑", "😲", "🙁", "😖", "😞", "😟", "😤", "😢", "😭", "😦", "😧",
	"😨", "😩", "🤯", "😬", "😰", "😱", "🥵", "🥶", "😳", "🤪", "😵", "🥴", "😠", "😡", "🤬", "😷", "🤒", "🤕", "🤢", "🤮",
	"🤧", "😇", "🥳", "🥺", "🤠", "🤡", "🤥", "🤫", "🤭", "🧐", "🤓", "😈", "👻", "💀", "👽", "🤖", "💩", "🙈", "🙉", "🙊",
	"👍", "👎", "👌", "✌", "🤞", "🤟", "🤘", "🤙", "👈", "👉", "👆", "👇", "✋", "🤚", "🖐", "🖖", "👋", "👏", "🙌", "🙏",
	"💪", "❤", "🧡", "💛", "💚", "💙", "💜", "🖤", "💔", "💕", "💖", "💯", "🔥", "✨", "🎉", "🎂", "🌹", "🌞", "⭐", "☕"};

void pane_tab(const bool b_stickers);

std::vector<std::string> sticker_files() {
	std::vector<std::string> out;
	auto scan = [&out](const std::string& dir) {
		DIR* d = opendir(dir.c_str());
		while (dirent* e = d ? readdir(d) : nullptr) {
			const std::string name = e->d_name;
			if (name.size() > 5 && name.substr(name.size() - 5) == ".webp") out.push_back(dir + "/" + name);
		}
		if (d) closedir(d);
	};
	/* recent: this chat's stickers, newest first, then the owner's saved ones */
	for (int i = int(cv.msgs.size()) - 1; i >= 0; i--) {
		const Msg& m = cv.msgs[size_t(i)];
		if (m.kind == "sticker" && !m.media.path.empty() && std::find(out.begin(), out.end(), m.media.path) == out.end()) out.push_back(m.media.path);
	}
	const size_t recent = out.size();
	scan(stickers_dir());
	const std::string top = stickers_dir().substr(0, stickers_dir().rfind('/'));
	scan(top);
	std::sort(out.begin() + long(recent), out.end());
	out.erase(std::unique(out.begin() + long(recent), out.end()), out.end());
	return out;
}

void send_sticker(const std::string& path) {
	if (!engine_up()) return;
	json req = {{"chat", cv.id}, {"kind", "sticker"}, {"path", path}, {"mime", "image/webp"}, {"width", 512}, {"height", 512}};
	if (!cv.reply_id.empty()) req["quoted_id"] = cv.reply_id;
	strip_set("", nullptr);
	request("send_media", req, [](const json*, const char* err) {
		if (err && alive()) confirm("sticker not sent", "The sticker could not be sent. Try again.", "ok", [] {});
	});
}

void fill_stickers() {
	const Zid grid = cv.pane_stickers;
	Zitems_control_clear(grid);
	const std::vector<std::string> files = sticker_files();
	for (const std::string& path : files) {
		const Zid img = Zimage_create();
		Zimage_set_stretch(img, Z_STRETCH_UNIFORM);
		Zelement_set_margin(img, Zthickness_uniform(H));
		const Zid c = Zgrid_view_item_create(nullptr);
		Zcontent_control_set_content(c, img);
		Zelement_set_name(c, path.c_str());
		Zitems_control_add(grid, c);
		image_file(path, 75.5f, 75.5f, false, [img](PixP p) { image_set(img, p); });
	}
	Zelement_set_visibility(Zelement_get_tag(grid) ? Zid(reinterpret_cast<intptr_t>(Zelement_get_tag(grid))) : Z_INVALID_ID,
		files.empty() ? Z_VISIBLE : Z_COLLAPSED);
}

void pane_show(const bool b_on) {
	if (!alive() || b_on == cv.b_pane) return;
	cv.b_pane = b_on;
	if (Zelement_is_valid(cv.emoji_icon)) Zsymbol_icon_set_symbol(cv.emoji_icon, b_on ? Z_SYMBOL_KEYBOARD : Z_SYMBOL_EMOJI);
	if (b_on) {
		/* the keyboard's height plus the app bar's, which the panel covers: the composer stays where it was */
		const float bar_h = Zelement_is_valid(cv.bar) ? Zelement_get_actual_height(cv.bar) : 0.0f;
		Zelement_set_height(cv.pane, g_kb_h + bar_h);
		Zelement_set_visibility(cv.pane, Z_VISIBLE);
		/* the panel is opaque and covers the app bar's place: the list ends at the composer */
		Zelement_set_visibility(cv.bar_room, Z_COLLAPSED);
		Zgrid_set_row_span(cv.area, 3);
		/* the focus off the box: the keyboard goes (zrnapp hides it when no text box has the focus); before the bar goes */
		Zelement_focus(cv.emoji_btn);
		if (Zelement_is_valid(cv.bar)) Zelement_set_visibility(cv.bar, Z_COLLAPSED);
		Zelement_set_translate(cv.pane, 0, 40);
		Zelement_animate(cv.pane, Z_PROP_TRANSLATE_Y, 0, 167, Z_EASE_OUT, 0);
		pane_tab(g_tab_stickers);
	} else {
		Zelement_set_visibility(cv.pane, Z_COLLAPSED);
		Zelement_set_visibility(cv.bar_room, Z_VISIBLE);
		Zgrid_set_row_span(cv.area, 5);
		if (Zelement_is_valid(cv.bar)) Zelement_set_visibility(cv.bar, Z_VISIBLE);
	}
}

void pane_tab(const bool b_stickers) {
	g_tab_stickers = b_stickers;
	Zelement_set_visibility(cv.pane_emoji, b_stickers ? Z_COLLAPSED : Z_VISIBLE);
	Zelement_set_visibility(Zelement_get_parent(cv.pane_stickers), b_stickers ? Z_VISIBLE : Z_COLLAPSED);
	Ztext_block_bind_foreground(cv.tab_emoji, b_stickers ? Z_RES_PHONE_BASE_MEDIUM_MID_COLOR : Z_RES_PHONE_FOREGROUND_BRUSH);
	Ztext_block_bind_foreground(cv.tab_stickers, b_stickers ? Z_RES_PHONE_FOREGROUND_BRUSH : Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
	if (b_stickers) fill_stickers();
}

void emoji_insert(const std::string& e) {
	std::string t = Ztext_box_get_text(cv.box);
	t += e;
	cv.last_text = t;
	Ztext_box_set_text(cv.box, t.c_str());
	Ztext_box_select(cv.box, int(t.size()), 0);
	send_enable();
}

Zid build_pane() {
	cv.pane = Zgrid_create();
	Zgrid_add_row(cv.pane, Z_GRID_AUTO, 0);
	Zgrid_add_row(cv.pane, Z_GRID_STAR, 1);
	Zpanel_set_background(cv.pane, Zcolor_rgba(0, 0, 0, 0));
	const Zid back = border(Z_RES_PHONE_CHROME_COLOR);
	Zgrid_place(cv.pane, back, 0, 0);
	Zgrid_set_row_span(back, 2);
	/* the two sections as WP pivot-style lowercase headers */
	const Zid tabs = Zstack_panel_create(Z_HORIZONTAL);
	Zelement_set_margin(tabs, Zthickness(M - H, H, M, 0));
	auto tab = [tabs](const char* label, Zid* tb, const bool b_stickers) {
		*tb = text(label, Z_RES_TEXT_STYLE_LARGE_FONT_SIZE, Z_FONT_SEMIBOLD, Z_RES_PHONE_FOREGROUND_BRUSH);
		Zelement_set_margin(*tb, Zthickness(H, H, M, H));
		const Zid t = tappable(*tb);
		click(t, [b_stickers] { pane_tab(b_stickers); });
		Zpanel_add(tabs, t);
	};
	tab("emoji", &cv.tab_emoji, false);
	tab("stickers", &cv.tab_stickers, true);
	Zgrid_place(cv.pane, tabs, 0, 0);
	/* emoji: 8 a row, a tap types it */
	cv.pane_emoji = Zgrid_view_create();
	Zlist_view_set_selection_mode(cv.pane_emoji, Z_LIST_VIEW_SELECTION_NONE);
	Zlist_view_set_is_item_click_enabled(cv.pane_emoji, true);
	const Zid ew = Zitems_wrap_grid_create(Z_HORIZONTAL);
	Zitems_wrap_grid_set_item_width(ew, 42.5f);
	Zitems_wrap_grid_set_item_height(ew, 42.5f);
	Zitems_control_set_items_panel(cv.pane_emoji, ew);
	Zelement_set_margin(cv.pane_emoji, Zthickness(H, 0, H, 0));
	for (const char* e : EMOJI) {
		const Zid t = text_px(e, 24, Z_FONT_NORMAL, Z_RES_PHONE_FOREGROUND_BRUSH);
		Zelement_set_horizontal_alignment(t, Z_HALIGN_CENTER);
		Zelement_set_vertical_alignment(t, Z_VALIGN_CENTER);
		const Zid c = Zgrid_view_item_create(nullptr);
		Zcontent_control_set_content(c, t);
		Zcontrol_set_horizontal_content_alignment(c, Z_HALIGN_CENTER);
		Zcontrol_set_vertical_content_alignment(c, Z_VALIGN_CENTER);
		Zelement_set_name(c, e);
		Zitems_control_add(cv.pane_emoji, c);
	}
	Zlist_view_on_item_click(cv.pane_emoji, [](const Zid l, const int index, void*) {
		emoji_insert(Zelement_get_name(Zitems_control_get_container(l, index)));
	}, nullptr);
	Zgrid_place(cv.pane, cv.pane_emoji, 0, 1);
	/* stickers: 4 a row, a tap sends it */
	const Zid sg = Zgrid_create();
	cv.pane_stickers = Zgrid_view_create();
	Zlist_view_set_selection_mode(cv.pane_stickers, Z_LIST_VIEW_SELECTION_NONE);
	Zlist_view_set_is_item_click_enabled(cv.pane_stickers, true);
	const Zid sw = Zitems_wrap_grid_create(Z_HORIZONTAL);
	Zitems_wrap_grid_set_item_width(sw, 85);
	Zitems_wrap_grid_set_item_height(sw, 85);
	Zitems_control_set_items_panel(cv.pane_stickers, sw);
	Zelement_set_margin(cv.pane_stickers, Zthickness(H, 0, H, 0));
	Zpanel_add(sg, cv.pane_stickers);
	const Zid none = empty_state("no stickers yet", "Stickers you get, and the ones saved from a message's menu, show here.");
	Zelement_set_margin(none, Zthickness(M, H, M, 0));
	Zpanel_add(sg, none);
	Zelement_set_tag(cv.pane_stickers, reinterpret_cast<void*>(intptr_t(none)));
	Zlist_view_on_item_click(cv.pane_stickers, [](const Zid l, const int index, void*) {
		send_sticker(Zelement_get_name(Zitems_control_get_container(l, index)));
	}, nullptr);
	Zgrid_place(cv.pane, sg, 0, 1);
	Zelement_set_visibility(sg, Z_COLLAPSED);
	Zelement_set_visibility(cv.pane, Z_COLLAPSED);
	return cv.pane;
}

/* ---- voice notes: hold the mic to record, slide left to cancel, let go to send (WhatsApp) ---- */
constexpr float REC_CANCEL = 100.0f;

void rec_ui(const bool b_on) {
	cv.b_rec = b_on;
	cancel(cv.rec_timer);
	Zelement_set_visibility(cv.rec_bar, b_on ? Z_VISIBLE : Z_COLLAPSED);
	Zelement_set_render_scale(cv.mic, b_on ? 1.35f : 1.0f);
	if (!b_on) return;
	Zelement_set_translate(cv.rec_hint, 0, 0);
	Zelement_set_opacity(cv.rec_hint, 1);
	Ztext_block_set_text(cv.rec_time, "0:00");
	struct P {
		static void step(const bool b_up) {
			if (!alive() || !cv.b_rec) return;
			Zelement_animate(cv.rec_dot, Z_PROP_OPACITY, b_up ? 1.0f : 0.2f, 500, Z_EASE_LINEAR, 0);
			Ztext_block_set_text(cv.rec_time, mmss(voice::record_ms() / 1000.0 - 0.49).c_str());
			cv.rec_timer = after(500, [b_up] { step(!b_up); });
		}
	};
	P::step(false);
}

void rec_hint_flash(const char* line) {
	Ztext_block_set_text(cv.rec_hint, line);
	Zelement_set_visibility(cv.rec_bar, Z_VISIBLE);
	Zelement_set_opacity(cv.rec_bar, 1);
	Ztext_block_set_text(cv.rec_time, "");
	after(1400, [] {
		if (!alive() || cv.b_rec) return;
		Zelement_set_visibility(cv.rec_bar, Z_COLLAPSED);
		Ztext_block_set_text(cv.rec_hint, "< slide to cancel");
	});
}

void rec_begin() {
	if (cv.b_channel || !engine_up()) return;
	if (!voice::can_record()) {
		confirm("can't record", "Recording needs pw-record (or parecord) and ffmpeg.", "ok", [] {});
		return;
	}
	voice::stop_all();
	cv.rec_dx = 0;
	cv.b_rec_cancelled = false;
	Ztext_block_set_text(cv.rec_hint, "< slide to cancel");
	if (!voice::record_start()) {
		confirm("can't record", "The microphone could not be opened.", "ok", [] {});
		return;
	}
	rec_ui(true);
}

void rec_drag(const float dx) {
	if (!cv.b_rec) return;
	cv.rec_dx += dx;
	const float x = std::min(0.0f, cv.rec_dx);
	Zelement_set_translate(cv.rec_hint, x, 0);
	Zelement_set_opacity(cv.rec_hint, std::max(0.2f, 1.0f + x / REC_CANCEL));
	if (cv.rec_dx < -REC_CANCEL) {
		cv.b_rec_cancelled = true;
		voice::record_cancel();
		rec_ui(false);
	}
}

void rec_end() {
	if (!cv.b_rec) return;
	const int ms = voice::record_ms();
	rec_ui(false);
	if (ms < 800) {
		voice::record_cancel();
		rec_hint_flash("hold to record, let go to send");
		return;
	}
	const std::string chat = cv.id, quoted = cv.reply_id;
	strip_set("", nullptr);
	voice::record_stop([chat, quoted](const voice::Note& n) {
		if (n.path.empty()) {
			confirm("not recorded", "The voice message could not be made. Try again.", "ok", [] {});
			return;
		}
		json req = {{"chat", chat}, {"kind", "audio"}, {"path", n.path}, {"mime", "audio/ogg; codecs=opus"}, {"ptt", true},
			{"seconds", n.seconds}, {"waveform", n.waveform}};
		if (!quoted.empty()) req["quoted_id"] = quoted;
		const std::string path = n.path;
		request("send_media", req, [path](const json*, const char* err) {
			if (err) confirm("not sent", "The voice message could not be sent. Try again.", "ok", [] {});
			unlink(path.c_str()); /* the engine keeps its own copy */
		});
	});
}

/* Recording, as the phone app: "SLIDE TO CANCEL" in the middle, the time large, a red mic at the right edge; the white mic
 * the finger holds stays at the left. */
Zid build_rec_bar() {
	cv.rec_bar = Zgrid_create();
	Zgrid_add_column(cv.rec_bar, Z_GRID_STAR, 1);
	Zgrid_add_column(cv.rec_bar, Z_GRID_AUTO, 0);
	Zgrid_add_column(cv.rec_bar, Z_GRID_AUTO, 0);
	Zpanel_set_background(cv.rec_bar, Zcolor_rgba(0, 0, 0, 0));
	const Zid back = border(Z_RES_PHONE_BACKGROUND_BRUSH);
	Zgrid_place(cv.rec_bar, back, 0, 0);
	Zgrid_set_column_span(back, 3);
	cv.rec_hint = text_px("SLIDE TO CANCEL", 14.0f, Z_FONT_SEMIBOLD, Z_RES_PHONE_BASE_MID_COLOR);
	Zelement_set_horizontal_alignment(cv.rec_hint, Z_HALIGN_CENTER);
	Zelement_set_vertical_alignment(cv.rec_hint, Z_VALIGN_CENTER);
	Zgrid_place(cv.rec_bar, cv.rec_hint, 0, 0);
	cv.rec_time = text_px("0:00", 30.0f, Z_FONT_SEMILIGHT, Z_RES_PHONE_FOREGROUND_BRUSH);
	Zelement_set_vertical_alignment(cv.rec_time, Z_VALIGN_CENTER);
	Zelement_set_margin(cv.rec_time, Zthickness(G, 0, 6, 0));
	Zgrid_place(cv.rec_bar, cv.rec_time, 1, 0);
	cv.rec_dot = Zcontent_control_create();
	const Zid red = symbol(Z_SYMBOL_MICROPHONE);
	Zelement_set_width(red, 22);
	Zelement_set_height(red, 22);
	Zcontent_control_set_content(cv.rec_dot, red);
	Zcontrol_set_foreground(cv.rec_dot, literal(0xFFE51400));
	Zelement_set_vertical_alignment(cv.rec_dot, Z_VALIGN_CENTER);
	Zgrid_place(cv.rec_bar, cv.rec_dot, 2, 0);
	Zelement_set_visibility(cv.rec_bar, Z_COLLAPSED);
	return cv.rec_bar;
}

/* the mic: a Thumb (press-and-drag) over the glyph; it keeps its drag from the list and from the swipe */
Zid build_mic() {
	const Zid g = Zgrid_create();
	Zelement_set_width(g, 38);
	Zelement_set_height(g, 38);
	Zelement_set_vertical_alignment(g, Z_VALIGN_TOP);
	/* the Segoe UI Symbol mic in the text colour, as the phone app */
	cv.mic = icon_holder(symbol(Z_SYMBOL_MICROPHONE), 22, Z_RES_PHONE_FOREGROUND_BRUSH);
	Zelement_set_horizontal_alignment(cv.mic, Z_HALIGN_CENTER);
	Zelement_set_vertical_alignment(cv.mic, Z_VALIGN_CENTER);
	Zpanel_add(g, cv.mic);
	const Zid t = Zthumb_create();
	Zcontrol_set_background(t, Zcolor_rgba(0, 0, 0, 0));
	Zthumb_on_drag_started(t, [](Zid, float, float, void*) { rec_begin(); }, nullptr);
	Zthumb_on_drag_delta(t, [](Zid, const float dx, float, void*) { rec_drag(dx); }, nullptr);
	Zthumb_on_drag_completed(t, [](Zid, float, float, void*) { rec_end(); }, nullptr);
	Zpanel_add(g, t);
	return g;
}

/* ---- documents ---- */
void send_document() {
	if (!engine_up()) return;
	const std::string chat = cv.id;
	pick_file([chat](const std::string& path) {
		if (!alive() || chat != cv.id) return;
		json req = {{"chat", chat}, {"kind", "document"}, {"path", path}};
		if (!cv.reply_id.empty()) req["quoted_id"] = cv.reply_id;
		strip_set("", nullptr);
		request("send_media", req, [](const json*, const char* err) {
			if (!err) return;
			confirm("not sent", !strcmp(err, "bad_request") ? "The file is too big to send (100 MB at most)." : "The document could not be sent. Try again.", "ok", [] {});
		});
	});
}

void focus_write(const bool b_on) {
	const std::string path = run_file("ztwhatsapp.focus");
	if (!b_on) {
		unlink(path.c_str());
		return;
	}
	const std::string temp = path + ".new";
	if (FILE* f = fopen(temp.c_str(), "w")) {
		fprintf(f, "%d %s\n", int(getpid()), cv.id.c_str());
		fclose(f);
		rename(temp.c_str(), path.c_str());
	}
}

/* The header of the phone app: the name in the title's Bold capitals, under it the status or the members in the subtle colour;
 * no picture (docs/REFERENCE-MEASUREMENTS.md). Caps 4 under the status bar, the second line's 23 under them. */
Zid build_header() {
	const Zid col = Zstack_panel_create(Z_VERTICAL);
	const Zid name = text_px(upper(cv.name), wp::TITLE_SIZE, Z_FONT_BOLD, Z_RES_PHONE_FOREGROUND_BRUSH);
	set_trim(name);
	Zelement_set_margin(name, Zthickness(0, 4.0f - 0.38f * wp::TITLE_SIZE, 0, 0));
	Zpanel_add(col, name);
	gap::continuum_target(cv.page, name);
	cv.status = text_px("", 15.0f, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MID_COLOR);
	set_trim(cv.status);
	Zelement_set_margin(cv.status, Zthickness(0.5f, 27.0f - 0.38f * 15.0f - (4.0f - 0.38f * wp::TITLE_SIZE + wp::TITLE_SIZE * 1.331f), 0, 0));
	Zpanel_add(col, cv.status);
	const Zid tap = tappable(col);
	Zelement_set_margin(tap, Zthickness(16.5f, 0, M, 4.0f));
	click(tap, [] {
		if (!cv.b_channel) nav(page_info, cv.id);
	});
	return tap;
}

Zid build_composer() {
	const Zid wrap = Zstack_panel_create(Z_VERTICAL);
	/* reply / edit strip (spec 2.2) */
	cv.strip = Zgrid_create();
	Zgrid_add_column(cv.strip, Z_GRID_PIXEL, H);
	Zgrid_add_column(cv.strip, Z_GRID_STAR, 1);
	Zgrid_add_column(cv.strip, Z_GRID_AUTO, 0);
	Zelement_set_margin(cv.strip, Zthickness(G, G, G, 0));
	Zgrid_place(cv.strip, border(Z_RES_PHONE_ACCENT_BRUSH), 0, 0);
	const Zid sc = Zstack_panel_create(Z_VERTICAL);
	Zelement_set_margin(sc, Zthickness(G, 0, 0, 0));
	cv.strip_label = text("", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_SEMIBOLD, Z_RES_PHONE_ACCENT_BRUSH);
	set_trim(cv.strip_label);
	Zpanel_add(sc, cv.strip_label);
	cv.strip_quote = text("", Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE, Z_FONT_NORMAL, Z_RES_PHONE_BASE_MEDIUM_MID_COLOR);
	set_trim(cv.strip_quote);
	Zpanel_add(sc, cv.strip_quote);
	Zgrid_place(cv.strip, sc, 1, 0);
	const Zid close = tappable(icon_holder(symbol(Z_SYMBOL_CANCEL), 20, Z_RES_PHONE_FOREGROUND_BRUSH));
	Zelement_set_width(close, 38);
	Zelement_set_height(close, 38);
	click(close, [] {
		if (!cv.edit_id.empty()) Ztext_box_set_text(cv.box, "");
		strip_set("", nullptr);
	});
	Zgrid_place(cv.strip, close, 2, 0);
	Zelement_set_visibility(cv.strip, Z_COLLAPSED);
	Zpanel_add(wrap, cv.strip);

	/* the phone app's composer: the mic at the left, the box as a bubble (its tail under the right end) to the right edge of
	 * the outgoing bubbles; the emoji button is in the app bar */
	cv.composer = Zgrid_create();
	Zgrid_add_column(cv.composer, Z_GRID_AUTO, 0);
	Zgrid_add_column(cv.composer, Z_GRID_STAR, 1);
	Zgrid_add_row(cv.composer, Z_GRID_AUTO, 0);
	Zgrid_add_row(cv.composer, Z_GRID_AUTO, 0);
	Zelement_set_margin(cv.composer, Zthickness(13.0f, 6.0f, LIST_RIGHT - 1.0f, 5.0f));
	cv.box = Ztext_box_create(nullptr);
	Ztext_box_set_placeholder_text(cv.box, "type a message");
	Ztext_box_set_accepts_return(cv.box, true);
	Ztext_box_set_text_wrapping(cv.box, Z_WRAP);
	Zcontrol_bind_font_size(cv.box, Z_RES_CONTENT_CONTROL_FONT_SIZE);
	Zelement_set_max_size(cv.box, Z_INFINITE, 4 * 27.0f + 14); /* four lines, then it scrolls */
	Zcontrol_set_border_thickness(cv.box, Zthickness_uniform(0));
	Zelement_set_margin(cv.box, Zthickness(11.0f, 0, 0, 0));
	Zgrid_place(cv.composer, cv.box, 1, 0);
	{
		/* the box's tail: its unfocused colour (0.8 of the brush, as the TextBox draws it), white while typing */
		const Zid tail = icon_holder(path_icon(TAIL_OUT), TAIL_W, Z_RES_TEXT_BOX_BACKGROUND_THEME_BRUSH);
		Zelement_set_height(tail, TAIL_H);
		Zelement_set_height(Zcontent_control_get_content(tail), TAIL_H);
		Zelement_set_horizontal_alignment(tail, Z_HALIGN_RIGHT);
		Zelement_set_margin(tail, Zthickness(0, -1, 5.0f, 0));
		Zelement_set_opacity(tail, 0.8f);
		Zelement_set_name(tail, "box-tail");
		Zgrid_place(cv.composer, tail, 1, 1);
		Zelement_on(cv.box, Z_EVENT_LOST_FOCUS, [](Zid, void*) {
			if (alive()) Zelement_set_opacity(Zelement_find_name(cv.composer, "box-tail"), 0.8f);
		}, nullptr);
	}
	Zgrid_place(cv.composer, build_mic(), 0, 0);
	/* over the box while recording; the mic stays beside it */
	const Zid rb = build_rec_bar();
	Zgrid_place(cv.composer, rb, 1, 0);
	Zpanel_add(wrap, cv.composer);
	/* a tap on the box while the panel is up: the keyboard comes back in its place */
	Zelement_on(cv.box, Z_EVENT_GOT_FOCUS, [](Zid, void*) {
		if (alive()) Zelement_set_opacity(Zelement_find_name(cv.composer, "box-tail"), 1.0f);
		/* the panel stays until the keyboard is up in its place (the input pane callback), so nothing jumps */
		if (cv.b_pane) after(400, [] {
			if (alive() && cv.b_pane) pane_show(false);
		});
	}, nullptr);
	on_text(cv.box, [] {
		std::string t = Ztext_box_get_text(cv.box);
		/* Enter sends: the newline the box took at the end is the key, not text */
		if (st.prefs.b_enter_sends && t.size() == cv.last_text.size() + 1 && !t.empty() && t.back() == '\n') {
			t.pop_back();
			Ztext_box_set_text(cv.box, t.c_str());
			cv.last_text = t;
			do_send();
			send_enable();
			return;
		}
		cv.last_text = t;
		send_enable();
		const long now = time(nullptr);
		if (!t.empty() && now - cv.last_typing >= 5 && engine_up()) {
			cv.last_typing = now;
			request("typing", {{"chat", cv.id}, {"state", "composing"}});
		}
	});
	return wrap;
}

Zid build_jump() {
	const Zid g = Zgrid_create();
	Zelement_set_width(g, 38 + H);
	Zelement_set_height(g, 38 + H);
	const Zid circle = border(Z_RES_PHONE_CHROME_COLOR);
	Zborder_set_corner_radius(circle, {19, 19, 19, 19});
	Zelement_set_width(circle, 38);
	Zelement_set_height(circle, 38);
	Zelement_set_horizontal_alignment(circle, Z_HALIGN_LEFT);
	Zelement_set_vertical_alignment(circle, Z_VALIGN_BOTTOM);
	const Zid ic = icon_holder(path_icon(glyph::chevron_down), 16, Z_RES_PHONE_FOREGROUND_BRUSH);
	Zelement_set_horizontal_alignment(ic, Z_HALIGN_CENTER);
	Zelement_set_vertical_alignment(ic, Z_VALIGN_CENTER);
	Zborder_set_child(circle, ic);
	Zpanel_add(g, circle);
	cv.badge = border(Z_RES_PHONE_ACCENT_BRUSH);
	Zelement_set_min_size(cv.badge, 19, 19);
	Zborder_set_padding(cv.badge, Zthickness(H, 0, H, 0));
	Zelement_set_horizontal_alignment(cv.badge, Z_HALIGN_RIGHT);
	Zelement_set_vertical_alignment(cv.badge, Z_VALIGN_TOP);
	cv.badge_text = text("", Z_RES_TEXT_STYLE_SMALL_FONT_SIZE, Z_FONT_SEMIBOLD, Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR);
	Zelement_set_horizontal_alignment(cv.badge_text, Z_HALIGN_CENTER);
	Zborder_set_child(cv.badge, cv.badge_text);
	Zelement_set_visibility(cv.badge, Z_COLLAPSED);
	Zpanel_add(g, cv.badge);
	const Zid tap = tappable(g);
	Zelement_set_horizontal_alignment(tap, Z_HALIGN_RIGHT);
	Zelement_set_vertical_alignment(tap, Z_VALIGN_BOTTOM);
	Zelement_set_margin(tap, Zthickness(0, 0, M, M));
	click(tap, [] {
		cv.unseen = 0;
		Zscroll_viewer_scroll_to_vertical_offset(cv.sv, end_offset());
		jump_update();
	});
	return tap;
}

}  // namespace

const std::string& open_chat_id() { return cv.id; }
bool chat_page_shown() { return alive() && Zframe_get_page(frame()) == cv.page; }

bool chat_back() {
	if (!alive() || !cv.b_pane) return false;
	pane_show(false);
	return true;
}

void open_chat(const std::string& id, const std::string& name_if_new, const bool b_channel) {
	cv.page = Z_INVALID_ID;
	cv.id = id;
	auto it = st.chats.find(id);
	cv.name = it != st.chats.end() ? it->second.name : name_if_new.empty() ? id.substr(0, id.find('@')) : name_if_new;
	cv.b_group = it != st.chats.end() ? it->second.b_group : id.find("@g.us") != std::string::npos;
	cv.b_channel = b_channel;
	nav(page_chat, id);
}

Zid page_chat(const Zid, void*) {
	const std::string id = cv.id, name = cv.name;
	const bool b_group = cv.b_group, b_channel = cv.b_channel;
	cv = ChatView();
	cv.id = id;
	cv.name = name;
	cv.b_group = b_group;
	cv.b_channel = b_channel;
	cv.page = Zpage_create();
	/* kept while info (S4) is over it, so back lands at the same scroll; a new chat is a new page */
	Zpage_set_navigation_cache_mode(cv.page, Z_NAVIGATION_CACHE_ENABLED);
	const Zid g = Zgrid_create();
	Zgrid_add_row(g, Z_GRID_AUTO, 0);
	Zgrid_add_row(g, Z_GRID_STAR, 1);
	Zgrid_add_row(g, Z_GRID_AUTO, 0);
	Zgrid_add_row(g, Z_GRID_AUTO, 0);
	Zgrid_add_row(g, Z_GRID_AUTO, 0);
	Zpage_set_content(cv.page, g);
	/* zrn-frosted-diffusion: the messages go on behind the header, the composer and the app bar, frosted there instead of cut at
	 * their edges; the list keeps room at its ends for them (Zscroll_viewer_get_content_insets) */
	const Zid area = Zgrid_create();
	cv.area = area;
	Zgrid_place(g, area, 0, 0);
	Zgrid_set_row_span(area, 5);
	cv.header = build_header();
	/* the frosted band is the screen's width; the name inside it keeps its own margins and tap area */
	const Zid header_band = Zgrid_create();
	Zpanel_add(header_band, cv.header);
	Zelement_set_frosted_diffusion(header_band, Z_FROSTED_DIFFUSION_NORMAL, Z_RES_PHONE_BACKGROUND_BRUSH);
	Zgrid_place(g, header_band, 0, 0);
	cv.list = Zlist_view_create();
	Zlist_view_set_selection_mode(cv.list, Z_LIST_VIEW_SELECTION_NONE);
	Zlist_view_set_is_item_click_enabled(cv.list, true);
	Zitems_control_set_items_panel(cv.list, Zitems_stack_panel_create(Z_VERTICAL));
	Zelement_set_margin(cv.list, Zthickness(LIST_LEFT, 0, LIST_RIGHT, 0));
	Zpanel_add(area, cv.list);
	cv.sv = Zitems_control_get_scroll_viewer(cv.list);
	cv.panel = Zitems_control_get_items_panel(cv.list);
	Zelement_set_margin(cv.panel, Zthickness(0, 0, 0, G));
	/* what sits on the list stays between the header and the composer */
	const Zid over = Zgrid_create();
	Zelement_set_hit_test_visible(over, false);
	Zgrid_place(g, over, 0, 1);
	cv.progress = top_progress();
	Zpanel_add(over, cv.progress);
	cv.older = top_progress();
	Zelement_set_visibility(cv.older, Z_COLLAPSED);
	Zpanel_add(over, cv.older);
	const Zid bn = banner();
	Zpanel_add(over, bn);
	cv.jump = build_jump();
	Zelement_set_visibility(cv.jump, Z_COLLAPSED);
	Zgrid_place(g, cv.jump, 0, 1);
	const Zid bar = app_bar();
	cv.bar = bar;
	Zpage_set_bottom_app_bar(cv.page, bar);
	cv.bar_room = Zgrid_create();
	Zelement_set_height(cv.bar_room, Zapp_bar_get_closed_height(bar));
	Zelement_set_hit_test_visible(cv.bar_room, false);
	Zgrid_place(g, cv.bar_room, 0, 4);
	if (!b_channel) {
		const Zid composer = build_composer();
		Zelement_set_frosted_diffusion(composer, Z_FROSTED_DIFFUSION_NORMAL, Z_RES_PHONE_BACKGROUND_BRUSH);
		Zgrid_place(g, composer, 0, 2);
		Zgrid_place(g, build_pane(), 0, 3);
		/* send, attach, emoji: the phone app's three, Segoe UI Symbol glyphs */
		cv.send = bar_button(bar, "send", symbol(Z_SYMBOL_SEND), [] { do_send(); });
		const Zid attach = Zapp_bar_button_create("attach", symbol(Z_SYMBOL_ATTACH));
		const Zid af = Zmenu_flyout_create();
		menu_item(af, "document", [] { send_document(); });
		menu_item(af, "sticker", [] {
			g_tab_stickers = true;
			pane_show(true);
		});
		Zbutton_set_flyout(attach, af);
		Zcommand_bar_add_primary_command(bar, attach);
		/* emoji / keyboard: opens the panel in the keyboard's place, or gives the keyboard back */
		cv.emoji_icon = symbol(Z_SYMBOL_EMOJI);
		cv.emoji_btn = bar_button(bar, "emoji", cv.emoji_icon, [] {
			if (cv.b_pane) {
				pane_show(false);
				Zelement_focus(cv.box);
			} else {
				pane_show(true);
			}
		});
		bar_secondary(bar, b_group ? "group info" : "view contact", [] { nav(page_info, cv.id); });
		bar_secondary(bar, "search", [] { nav(page_search); });
		send_enable();
	} else {
		bar_secondary(bar, "unfollow", [] {
			confirm("unfollow?", "You will no longer see this channel's updates.", "unfollow", [] {
				request("channel_unfollow", {{"id", cv.id}}, [](const json*, const char* err) {
					if (err) return;
					load_channels();
					go_back();
				});
			});
		});
	}
	Zlist_view_on_item_click(cv.list, [](const Zid list, const int index, void*) {
		const std::string mid = Zelement_get_name(Zitems_control_get_container(list, index));
		if (mid.empty()) return;
		const int i = index_of(mid);
		if (i < 0) return;
		/* with Holding (G2) a tap opens only a failed message's retry menu */
		if (gap::have_holding() && cv.msgs[size_t(i)].status != "failed") return;
		message_menu(mid);
	}, nullptr);
	Zscroll_viewer_on_scroll(cv.sv, [](Zid, ZScrollEventType, const double v, void*) {
		if (!alive()) return;
		if (v < Zscroll_viewer_get_viewport_height(cv.sv)) load_older();
		jump_update();
	}, nullptr);
	Zpage_on_navigated_to(cv.page, [](Zid, void*) {
		focus_write(true);
		mark_read();
		land();
		if (cv.b_end_pending) {
			cv.b_end_pending = false;
			Zroot_update_layout(g_root);
			Zscroll_viewer_scroll_to_vertical_offset(cv.sv, end_offset());
			jump_update();
		}
	}, nullptr);
	Zpage_on_navigated_from(cv.page, [](Zid, void*) {
		focus_write(false);
		if (cv.b_rec) {
			voice::record_cancel();
			rec_ui(false);
		}
		voice::pause();
		if (engine_up()) request("typing", {{"chat", cv.id}, {"state", "paused"}});
	}, nullptr);
	/* G5: the keyboard's height sizes the panel; the keyboard coming up takes the panel's place */
	gap::on_input_pane([](const float h, void*) {
		if (h <= 0) return;
		g_kb_h = h;
		/* only the keyboard coming up for the text box takes the panel's place: the one going away (it still reports its
		 * height while it leaves, as the panel opens) does not close the panel it makes room for */
		if (alive() && cv.b_pane && Zroot_get_focus(g_root) == cv.box) pane_show(false);
	}, nullptr);
	header_status();
	/* history: the header shows at once, the list fills when the page arrives */
	auto it = st.chats.find(id);
	const int unread = it != st.chats.end() ? it->second.unread : 0;
	auto done = [id, unread](const json* r, const char* err) {
		if (!alive() || id != cv.id) return;
		Zelement_set_visibility(cv.progress, Z_COLLAPSED);
		cv.b_loaded = true;
		if (err || !r->contains("messages")) return;
		cv.b_more = jbool(*r, "more");
		fill_first((*r)["messages"], unread);
		if (st.typing.count(cv.id) && st.typing[cv.id]) typing_show(true);
	};
	if (b_channel) request("channel_messages", {{"id", id}, {"limit", 50}}, done);
	else {
		request("messages", {{"chat", id}, {"limit", PAGE_SIZE}}, done);
		request("subscribe_presence", {{"chat", id}});
		if (b_group)
			request("group_info", {{"chat", id}}, [id](const json* r, const char* err) {
				if (err || !alive() || id != cv.id || !r->contains("participants")) return;
				std::string s;
				for (auto& p : (*r)["participants"]) {
					std::string n = jstr(p, "name");
					n = n.substr(0, n.find(' '));
					if (n.empty()) continue;
					if (!s.empty()) s += ", ";
					s += n;
					if (s.size() > 120) break;
				}
				cv.members = s;
				header_status();
			});
	}
	return cv.page;
}

void chat_on_message(const json& o) {
	if (!alive() || jstr(o, "chat") != cv.id || !o.contains("message") || !cv.b_loaded) return;
	Msg m = msg_from(o["message"]);
	if (m.chat.empty()) m.chat = cv.id;
	if (m.kind == "reaction" || m.id.empty()) return;
	int known = index_of(m.id);
	/* our own send comes back as an event, maybe before the reply: it is the local bubble */
	if (known < 0 && m.b_from_me)
		for (int i = int(cv.msgs.size()) - 1; i >= 0 && known < 0; i--)
			if (m.kind == "text" && cv.msgs[size_t(i)].id.rfind("local-", 0) == 0 && cv.msgs[size_t(i)].text == m.text) {
				known = i;
				cv.msgs[size_t(i)].id = m.id;
				cv.rows[size_t(i)].id = m.id;
				Zelement_set_name(cv.rows[size_t(i)].container, m.id.c_str());
			}
	if (m.kind == "hidden") {
		if (known >= 0) delete_row(known);
		return;
	}
	if (known >= 0) {
		Msg& old = cv.msgs[size_t(known)];
		/* a voice note's length and waveform can come later (the keys asked of the phone for an old one) */
		const bool b_same_shape = old.kind == m.kind && old.b_deleted == m.b_deleted && old.media.path == m.media.path &&
			old.media.seconds == m.media.seconds && old.media.waveform == m.media.waveform;
		if (b_same_shape) {
			Row& r = cv.rows[size_t(known)];
			if (Zelement_is_valid(r.body) && m.text != old.text) Ztext_block_set_text(r.body, m.text.c_str());
			if (Zelement_is_valid(r.edited)) Zelement_set_visibility(r.edited, m.b_edited ? Z_VISIBLE : Z_COLLAPSED);
			if (old.status == "pending" && m.status != "pending") old.status = m.status;
			else if (!m.status.empty()) old.status = m.status;
			old.text = m.text;
			old.b_edited = m.b_edited;
			tick_set(r, old);
			return;
		}
		/* a deletion or a downloaded picture: the row is built again in place */
		const Zid c = cv.rows[size_t(known)].container;
		const int at = Zitems_control_index_from_container(cv.list, c);
		cv.msgs[size_t(known)] = std::move(m);
		Row r = build_row(known);
		Zitems_control_remove_at(cv.list, at);
		Zpanel_insert(cv.panel, at, r.container);
		cv.rows[size_t(known)] = r;
		row_spacing(known);
		return;
	}
	if (Zelement_is_valid(cv.typing) && !m.b_from_me) typing_show(false);
	const bool b_mine = m.b_from_me;
	cv.msgs.push_back(std::move(m));
	append_row(true, b_mine);
	if (chat_page_shown()) mark_read();
}

void chat_on_receipt(const json& o) {
	if (!alive() || jstr(o, "chat") != cv.id || !o.contains("ids")) return;
	const std::string s = jstr(o, "status");
	for (auto& idv : o["ids"]) {
		if (!idv.is_string()) continue;
		const int i = index_of(idv.get<std::string>());
		if (i < 0) continue;
		cv.msgs[size_t(i)].status = s;
		tick_set(cv.rows[size_t(i)], cv.msgs[size_t(i)]);
	}
}

void chat_on_presence(const std::string& chat) {
	if (!alive() || chat != cv.id) return;
	header_status();
	typing_show(st.typing[chat]);
}

/* the viewer downloaded a picture: the bubble shows it in full too */
void chat_media_downloaded(const std::string& chat, const std::string& id, const std::string& path) {
	if (!alive() || cv.id != chat) return;
	const int k = index_of(id);
	if (k < 0 || !cv.msgs[size_t(k)].media.path.empty()) return;
	cv.msgs[size_t(k)].media.path = path;
	image_show_full(id);
}

}  // namespace wa

namespace wa {
namespace {
ZRect in_root(const Zid e) {
	Zroot_update_layout(g_root);
	ZRect b = Zelement_get_bounds(e);
	const ZPoint p = Zelement_to_root(e, {0, 0});
	b.x = p.x;
	b.y = p.y;
	return b;
}
}  // namespace

/* The self test's walk through the chat: the keyboard (G5), the panel in its place, a swipe to reply, a voice note
 * playing, the recorder. Header positions are printed so a run shows whether the header stayed put. */
void chat_selftest(const char*, std::function<void(const char*)> shot, std::function<void(int)> run) {
	if (!alive()) return;
	Zscroll_viewer_scroll_to_vertical_offset(cv.sv, end_offset());
	run(400);
	const ZRect h0 = in_root(cv.header);
	shot("2a-end");
	/* the keyboard: a tap on the box, the root inset as zrnapp does, then typing */
	{
		const ZRect bx = in_root(cv.box);
		Zroot_inject_pointer(g_root, Z_POINTER_DOWN, bx.x + 30, bx.y + 10);
		Zroot_inject_pointer(g_root, Z_POINTER_UP, bx.x + 30, bx.y + 10);
	}
	Zroot_set_input_pane_height(g_root, 280);
	run(300);
	for (const char* w : {"h", "e", "l", "l", "o", " ", "t", "h", "e", "r", "e"}) {
		Zroot_inject_text(g_root, w);
		run(30);
	}
	run(600);
	const ZRect h1 = in_root(cv.header), c1 = in_root(cv.composer);
	printf("selftest: header y %.1f -> %.1f with the keyboard; composer bottom %.1f (root %.1f)\n", double(h0.y), double(h1.y),
		double(c1.y + c1.height), double(740 - 280));
	shot("2b-keyboard");
	/* the panel in the keyboard's place */
	g_kb_h = 280;
	pane_show(true);
	Zroot_set_input_pane_height(g_root, 0);
	run(600);
	const ZRect c2 = in_root(cv.composer);
	printf("selftest: composer bottom %.1f with the panel (was %.1f)\n", double(c2.y + c2.height), double(c1.y + c1.height));
	shot("2c-emoji");
	pane_tab(true);
	run(800);
	shot("2d-stickers");
	pane_show(false);
	run(400);
	/* swipe the last incoming text bubble to the right */
	for (int i = int(cv.msgs.size()) - 1; i >= 0; i--) {
		const Msg& m = cv.msgs[size_t(i)];
		if (m.b_from_me || m.kind != "text") continue;
		scroll_center(i);
		run(300);
		const ZRect b = in_root(cv.rows[size_t(i)].block);
		const float x = b.x + 20, y = b.y + b.height / 2;
		Zroot_inject_pointer(g_root, Z_POINTER_DOWN, x, y);
		for (int k = 1; k <= 8; k++) {
			Zroot_inject_pointer(g_root, Z_POINTER_MOVE, x + float(k) * 10, y + 1);
			run(16);
		}
		shot("2e-swipe");
		Zroot_inject_pointer(g_root, Z_POINTER_UP, x + 80, y + 1);
		run(400);
		printf("selftest: swipe reply %s\n", cv.reply_id == m.id ? "set" : "NOT set");
		shot("2f-reply");
		break;
	}
	strip_set("", nullptr);
	/* the voice notes, documents and sticker at the end of the fake engine's first person */
	if (st.chats.count("61400000001@s.whatsapp.net") && cv.id != "61400000001@s.whatsapp.net") {
		open_chat("61400000001@s.whatsapp.net");
		run(2500);
	}
	Zscroll_viewer_scroll_to_vertical_offset(cv.sv, end_offset());
	run(1500);
	shot("2g-media");
	for (int i = int(cv.msgs.size()) - 1; i >= 0; i--)
		if (cv.msgs[size_t(i)].kind == "audio" && !cv.msgs[size_t(i)].b_from_me) {
			voice_toggle(cv.msgs[size_t(i)].id);
			run(2500);
			shot("2h-voice-playing");
			voice::pause();
			run(200);
			break;
		}
	/* the recorder's bar, without a microphone: the UI alone */
	cv.b_rec = false;
	rec_ui(true);
	run(1200);
	shot("2i-recording");
	rec_ui(false);
	run(200);
}
}  // namespace wa
