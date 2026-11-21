// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * WhatsApp for ztOS, the UI on zrnui v2 (docs/UI-SPEC.md is the design authority).
 *
 * The UI is a client of the engine (IPC.md). One thread runs zrnui; the io thread
 * (io.cpp) owns the socket, the timers, the file watches and image decoding, and hands
 * work back to the UI thread through wa::post. Never log message text, names or numbers.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <zrnui/controls.h>
#include <zrnui/core.h>
#include <zrnui/element.h>
#include <zrnui/host.h>
#include <zrnui/resources.h>

#include "gaps.h"

namespace wa {

using json = nlohmann::json;
using Reply = std::function<void(const json* result, const char* error)>;

/* ---- the grid of the spec (section 0): 19 / 9.5, 4.75 inside bubbles ---- */
constexpr float M = 19.0f;
constexpr float G = 9.5f;
constexpr float H = 4.75f;
constexpr int PAGE_SIZE = 60;

/* ---- the Windows Phone WhatsApp page, measured on phone screenshots scaled to 360 points (docs/REFERENCE-MEASUREMENTS.md).
 * The window starts under the shell's status bar; these are from the window's top. ---- */
namespace wp {
constexpr float LEFT = 15.0f;          /* the left edge of the title, the headers and the rows */
constexpr float RIGHT = 19.0f;         /* the right edge of the rows' times and counts */
constexpr float TITLE_SIZE = 15.5f;    /* WHATSAPP: Bold, its caps 11 tall */
constexpr float TITLE_TOP = 5.8f;      /* the top of its line: the caps 12 under the status bar, as on the phone */
constexpr float HEADER_SIZE = 38.5f;   /* chats / status: SemiLight, the h 28.5 tall */
constexpr float STRIP_TOP = 30.4f;     /* the pivot's strip (PivotPortraitThemePadding top with no pivot title) */
constexpr float ITEM_TOP = -6.0f;      /* the pivot item under the strip: the first avatar 17 under the headers' baseline */
constexpr float ROW_PITCH = 64.0f;     /* a chat row: the 49 point picture and 7.5 above and below */
constexpr float AVATAR = 49.0f;
/* The ztOS status bar is 28 points, the phone's 24: positions measured from the top of the phone's screen are 4 lower here. */
}  // namespace wp

/* ---- io.cpp: the engine, timers, the worker ---- */
void io_start(void* wl_display);
void io_stop();
/** Drains the UI queue by hand (the self test, which runs without a window). */
void io_pump();
/** Sends a command; the reply runs on the UI thread. Without the engine the reply gets "not_connected" at once. */
void request(const char* cmd, json fields = json::object(), Reply reply = nullptr);
bool engine_up();
/** The engine went away: every reply still waiting gets "not_connected" (UI thread). */
void fail_waiting();
/** Runs `fn` on the UI thread (from any thread). */
void post(std::function<void()> fn);
/* UI thread, later, when no animation runs (engine replies and events go this way too): see io.cpp */
void post_idle(std::function<void()> fn);
/* a page transition, a fling, a swipe, a tween, a finger dragging, or the shell's launch zoom is on screen */
bool animating();
/** Runs `fn` on the UI thread after `ms`; returns an id for cancel. */
int after(int ms, std::function<void()> fn);
void cancel(int timer);
/** Runs `fn` on the worker thread (decoding, file reads). */
void work(std::function<void()> fn);
std::string run_file(const char* name);
/** Starts a login unit of the installed program (systemctl --user); false when this copy has none (the caller spawns). */
bool start_unit(const char* unit);

/* ---- main.cpp: what the io thread reports ---- */
void on_event(const json& event);
void on_engine(bool up);
void on_run_dir_change();
void on_shell_config_change();

/* ---- state ---- */
struct Chat {
	std::string id, name, last_text;
	bool b_group = false, b_last_from_me = false, b_muted = false, b_pinned = false, b_archived = false;
	int unread = 0;
	long last_ts = 0;
};
struct Media {
	std::string mime, thumb, path, file_name, waveform;
	int width = 0, height = 0, seconds = 0;
	long size = 0;
	bool b_ptt = false, b_played = false, b_animated = false;
};
struct Reaction {
	std::string emoji;
	int count = 0;
};
struct Msg {
	std::string id, chat, sender, sender_name, kind, text, status, quoted_id;
	bool b_from_me = false, b_starred = false, b_edited = false, b_deleted = false, b_media = false;
	long ts = 0;
	Media media;
	std::vector<Reaction> reactions;
};
struct StatusItem {
	std::string id, text, kind, thumb, path;
	long ts = 0;
	bool b_seen = false;
};
struct StatusPerson {
	std::string sender, name;
	long last_ts = 0;
	int unseen = 0;
	std::vector<StatusItem> items;
};
struct Contact {
	std::string jid, name, phone;
};
struct Channel {
	std::string id, name, about;
};
struct Prefs {
	bool b_notify = true, b_preview = true, b_enter_sends = false, b_stickers = true, b_green = false;
};

struct State {
	std::map<std::string, Chat> chats;
	std::vector<StatusPerson> statuses;
	std::vector<Channel> channels;
	std::vector<Contact> contacts;
	bool b_chats_loaded = false, b_statuses_loaded = false, b_channels_loaded = false, b_contacts_loaded = false;
	std::string connection = "connecting", pair_state, pair_qr, pair_code, pair_error, self_jid;
	bool b_logged_out = false;
	std::map<std::string, std::string> presence;  /* chat -> line */
	std::map<std::string, bool> typing;           /* chat -> composing */
	Prefs prefs;
};
extern State st;
extern Zid g_root;

/* ---- helpers (main.cpp) ---- */
std::string jstr(const json& o, const char* key);
long jint(const json& o, const char* key);
bool jbool(const json& o, const char* key);
Chat chat_from(const json& o);
Msg msg_from(const json& o);
std::vector<const Chat*> chats_sorted(bool archived);
std::string time_hm(long ts);
std::string time_short(long ts);
std::string day_label(long ts);
std::string one_line(std::string text);
std::string first_letter(const std::string& name);
std::string lower(std::string s);
std::string upper(std::string s);
bool emoji_only(const std::string& text, int* count);
void prefs_save();
void load_chats();
void load_statuses();
void load_channels();
void load_contacts();

/* ---- navigation (main.cpp) ---- */
Zid frame();
void go_back();
void nav(ZPageFactory factory, const std::string& parameter = "", ZPageTransition transition = Z_TRANSITION_TURNSTILE);
void open_chat(const std::string& id, const std::string& name_if_new = "", bool b_channel = false);
/* back's step in the chat: closes the emoji / sticker panel that stands in the keyboard's place (true when it did) */
bool chat_back();
void show_link_root();
void show_main_root(const std::string& then_chat = "");
const std::string& page_param(Zid page);

/* ---- look.cpp: the controls of the spec, bound to theme keys ---- */
Zid text(const std::string& s, ZThemeResource size, ZFontFamily family, ZThemeResource fg);
Zid text_px(const std::string& s, float size, ZFontFamily family, ZThemeResource fg);
void set_trim(Zid tb);
Zid icon_holder(Zid icon, float size, ZThemeResource fg);
Zid path_icon(const std::string& data);
Zid symbol(ZSymbol s);
Zid tappable(Zid content);
Zid border(ZThemeResource background);
Zid avatar(const std::string& jid, const std::string& name, float size, bool b_group);
Zid page_frame(Zid page, const char* title, const char* header, Zid* body);
Zid top_progress();
Zid empty_state(const char* heading, const char* line);
Zid banner();
void banners_update();
Zid app_bar();
Zid bar_button(Zid bar, const char* label, Zid icon, std::function<void()> click, bool b_secondary = false);
Zid bar_secondary(Zid bar, const char* label, std::function<void()> click);
void bar_enable(Zid bar, bool b_on);
Zid menu_item(Zid menu, const char* label, std::function<void()> click, bool b_enabled = true);
void confirm(const char* title, const char* content, const char* primary, std::function<void()> yes);
void click(Zid button, std::function<void()> fn);
void on_text(Zid text_box, std::function<void()> fn);
void list_entrance(Zid list);
void row_flash(Zid row);
void theme_apply();
ZColor literal(uint32_t argb);

/* path data of the glyphs (look.cpp) */
namespace glyph {
extern const std::string tick_clock, tick_one, tick_two, chevron_down, people, mute, pin, emoji, attach, mic, page, alert_disc,
	play_disc, star, chat_badge, ring;
}

/* ---- media.cpp ---- */
struct Pix {
	int w = 0, h = 0;
	std::vector<uint8_t> px;
};
using PixP = std::shared_ptr<const Pix>;
std::vector<uint8_t> unbase64(const std::string& in);
/** Decodes a file (JPEG, PNG or WebP, first frame) on the worker, fitted to max_w x max_h logical px times the scale, cached. */
void image_file(const std::string& path, float max_w, float max_h, bool b_cover, std::function<void(PixP)> done);
void image_bytes(std::vector<uint8_t> bytes, float w, float h, std::function<void(PixP)> done);
void image_set(Zid image, const PixP& pix);
void picture_want(const std::string& jid, Zid image);
void picture_changed(const std::string& jid);
float ui_scale();
/** The phone reaches the internet over Wi-Fi or USB (not mobile data): pictures then download by themselves. */
bool unmetered();
/** The viewer downloaded a chat's picture (page_chat.cpp). */
void chat_media_downloaded(const std::string& chat, const std::string& id, const std::string& path);
PixP qr_pix(const std::string& text, int* modules);
std::string stickers_dir();

/* ---- voice.cpp: voice notes through the system's default devices (pw-record / pw-play, ffmpeg) ---- */
namespace voice {
struct Note {
	std::string path, waveform; /* an Opus OGG (empty when encoding failed), 64 values 0-100 in base64 */
	int seconds = 0;
};
bool can_record();
bool record_start();
int record_ms();
void record_cancel();
void record_stop(std::function<void(const Note&)> done);
/** Plays `path` from `from` seconds; `tick` runs on the UI thread every 100 ms and once with b_playing false at the end. */
void play(const std::string& key, const std::string& path, double from, std::function<void(double pos, double dur, bool b_playing)> tick);
void pause();
void stop_all();
bool playing(const std::string& key);
std::vector<int> waveform_decode(const std::string& b64);
}  // namespace voice

/* ---- page_files.cpp: a document to send, picked from the home folder ---- */
/** Opens the picker; `picked` gets the file's path on the UI thread (never when cancelled). */
void pick_file(std::function<void(const std::string&)> picked);
Zid page_pick_file(Zid frame, void* p);
/** Opens a file with the desktop's program for it (xdg-open), detached. */
void open_external(const std::string& path);
std::string human_size(long bytes);

/* ---- pages ---- */
Zid page_main(Zid frame, void* p);
Zid page_chat(Zid frame, void* p);
Zid page_new_chat(Zid frame, void* p);
Zid page_info(Zid frame, void* p);
Zid page_settings(Zid frame, void* p);
Zid page_account(Zid frame, void* p);
Zid page_link(Zid frame, void* p);
Zid page_media(Zid frame, void* p);
Zid page_status_view(Zid frame, void* p);
Zid page_new_status(Zid frame, void* p);
Zid page_search(Zid frame, void* p);
Zid page_starred(Zid frame, void* p);
Zid page_archived(Zid frame, void* p);
Zid page_follow(Zid frame, void* p);

/* what the pages are told */
void main_chats_changed(const std::string& id);
void main_statuses_changed();
void main_channels_changed();
void main_engine_changed();
void chat_on_message(const json& event);
void chat_on_receipt(const json& event);
void chat_on_presence(const std::string& chat);
void link_changed();
void new_chat_contacts_changed();
const std::string& open_chat_id();
bool chat_page_shown();
/** The self test's walk through the chat (keyboard, panels, swipe), shooting each step. */
void chat_selftest(const char* dir, std::function<void(const char*)> shot, std::function<void(int)> run);

/* media viewer parameters */
struct MediaView {
	std::string chat, id, path, caption;
	PixP thumb;
};
extern MediaView g_media_view;
extern int g_status_person;

}  // namespace wa
