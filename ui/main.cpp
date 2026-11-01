// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * WhatsApp for ztOS, in libzrnui, in the system's style (Metro or DotMatrix). An unofficial client.
 *
 * All data comes from the engine over the socket in IPC.md. One binary, three
 * roles, because a .zexec holds one program:
 *	zrn-whatsapp [JID]	the app; starts the other two if they are not running
 *	zrn-whatsapp --daemon ...	the engine (ztwhatsappd)
 *	zrn-whatsapp --notifier ...	the notifier (ztwhatsapp-notify)
 *
 * Never log message text, names or numbers.
 */
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <webp/decode.h>
#include <webp/demux.h>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>
#include <zrnui.hpp>
#include "qr.h"

int ztwhatsappd_main(int argc, char **argv);
int ztwhatsapp_notify_main(int argc, char **argv);

namespace {
using json = nlohmann::json;
using zrn::ui::Node;
using zrn::ui::Variant;

constexpr uint32_t WA_GREEN = 0x25d366;
constexpr int PAGE_SIZE = 60;

zrn::ui::App *app;
/* rows are bound on libzrnui's row thread: what bind reads changes under this */
struct Hold { Hold() { zui_list_lock(app->get(), true); } ~Hold() { zui_list_lock(app->get(), false); } };

std::string run_file(const char *name) {
	const char *run = getenv("XDG_RUNTIME_DIR");
	return std::string(run ? run : "/tmp") + "/" + name;
}

template <class T> std::string s(const json &o, const char *k) {
	auto it = o.find(k);
	return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}
std::string jstr(const json &o, const char *k) { return s<int>(o, k); }
long jint(const json &o, const char *k) {
	auto it = o.find(k);
	return it != o.end() && it->is_number() ? it->get<long>() : 0;
}
bool jbool(const json &o, const char *k) {
	auto it = o.find(k);
	return it != o.end() && it->is_boolean() && it->get<bool>();
}

std::string when_text(long ts) {
	time_t t = ts, now = time(nullptr);
	struct tm a, b;
	localtime_r(&t, &a);
	localtime_r(&now, &b);
	char out[32];
	if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) strftime(out, sizeof(out), "%H:%M", &a);
	else if (now - ts < 6 * 86400) strftime(out, sizeof(out), "%a", &a);
	else strftime(out, sizeof(out), "%d %b", &a);
	return out;
}
std::string day_text(long ts) {
	time_t t = ts;
	struct tm a;
	localtime_r(&t, &a);
	char out[48];
	strftime(out, sizeof(out), "%A %-d %B", &a);
	return out;
}
std::string first_letter(const std::string &n) {
	if (n.empty() || isdigit(static_cast<unsigned char>(n[0])) || n[0] == '+') return "#";
	size_t k = 1;
	while (k < n.size() && (static_cast<unsigned char>(n[k]) & 0xc0) == 0x80) k++;
	std::string l = n.substr(0, k);
	if (k == 1) l[0] = char(toupper(static_cast<unsigned char>(l[0])));
	return l;
}
std::string one_line(std::string t) {
	std::replace(t.begin(), t.end(), '\n', ' ');
	if (t.size() > 160) {
		size_t c = 160;
		while (c && (static_cast<unsigned char>(t[c]) & 0xc0) == 0x80) c--;
		t.resize(c);
	}
	return t;
}
std::vector<uint8_t> unbase64(const std::string &in) {
	std::vector<uint8_t> out;
	unsigned v = 0;
	int bits = 0;
	for (unsigned char c : in) {
		int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
		if (d < 0) continue;
		v = (v << 6) | unsigned(d);
		bits += 6;
		if (bits >= 8) { bits -= 8; out.push_back(uint8_t(v >> bits)); }
	}
	return out;
}

/* ---- what is shown ---- */
struct Chat {
	std::string id, name, last_text;
	bool is_group = false, last_from_me = false, muted = false, pinned = false, archived = false;
	int unread = 0;
	long last_ts = 0;
};
struct Msg {
	std::string id, sender_name, text, kind, status, thumb, path;
	std::string quoted;
	bool from_me = false, starred = false, system = false, image = false, sticker = false, edited = false;
	long ts = 0;
	int w = 0, h = 0;
	zui_pixmap *pix = nullptr;
	~Msg() { if (pix) zui_pixmap_free(pix); }
};
struct StatusItem { std::string id, text, kind, thumb, path; long ts = 0; bool seen = false; };
struct StatusPerson { std::string sender, name; long last_ts = 0; int unseen = 0; std::vector<StatusItem> items; };
struct Contact { std::string jid, name, phone; };
struct Found { std::string chat, name, text; long ts = 0; };

std::map<std::string, Chat> all_chats;
std::vector<Chat> chats, archived;
std::deque<std::unique_ptr<Msg>> msgs;
std::vector<StatusPerson> statuses;
std::vector<Contact> contacts, contacts_shown;
std::vector<Found> starred, found;
std::map<std::string, zui_pixmap *> pictures;
std::vector<std::string> picture_queue;
bool picture_busy;

std::string connection = "connecting", pair_state = "connecting", pair_code, pair_error, pair_qr, self_jid;
std::string open_id, open_name, presence_line, pending_open;
struct Channel { std::string id, name, about; };
std::vector<Channel> channels;
std::map<std::string, zui_pixmap *> sticker_cache;
std::string reply_id, edit_id;
bool read_only, pref_enter = false, pref_stickers = true, pref_notify = true, pref_preview = true;
Node message_page, message_box, stickers_page, stickers_box, settings_page, settings_box, follow_page, follow_field, list_channels, note_channels;
void save_sticker(const std::string &id);
void do_send();
std::string prefs_path();
void load_chats();
bool open_group, older_more, older_pending, opening, foreground, follow_accent, logout_ask;
int status_who = -1, status_at = 0;
std::string search_text, new_text, phone_text, post_note;
long last_typing;

Node main_page, tabs, list_chats, list_status, list_starred, list_archived, note_chats;
Node chat_page, list_msgs, field_draft;
Node link_page, link_box, search_page, search_box, new_page, list_new, new_note;
Node status_page, status_box, post_page, post_note_n, info_page, info_box, account_page, account_box, image_page, image_box;

/* ---- the socket ---- */
using Reply = std::function<void(const json *result, const char *error)>;
int sock_fd = -1, next_id = 1;
std::string in_buf, out_buf;
std::map<int, Reply> waiting;
bool services_started;

void flush_out() {
	while (sock_fd >= 0 && !out_buf.empty()) {
		ssize_t n = send(sock_fd, out_buf.data(), out_buf.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n <= 0) break;
		out_buf.erase(0, size_t(n));
	}
}
void request(const char *cmd, json fields = json::object(), Reply reply = nullptr) {
	if (sock_fd < 0) {
		if (reply) reply(nullptr, "not_connected");
		return;
	}
	int id = next_id++;
	fields["id"] = id;
	fields["cmd"] = cmd;
	if (reply) waiting[id] = std::move(reply);
	out_buf += fields.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
	flush_out();
}

/* ---- pictures of people: one fetch at a time ---- */
void refresh_rows_of(const std::string &jid);
zui_pixmap *picture_of(const std::string &jid) {
	auto it = pictures.find(jid);
	if (it != pictures.end()) return it->second;
	pictures[jid] = nullptr;
	picture_queue.push_back(jid);
	return nullptr;
}
void pictures_next() {
	if (picture_busy || picture_queue.empty() || sock_fd < 0) return;
	std::string jid;
	{
		Hold hold;
		jid = picture_queue.back();
		picture_queue.pop_back();
	}
	picture_busy = true;
	request("profile_picture", {{"jid", jid}, {"preview", true}}, [jid](const json *r, const char *err) {
		picture_busy = false;
		std::string path = err ? "" : jstr(*r, "path");
		if (!path.empty()) {
			/* off the UI thread: JPEG decoding is the one slow thing here */
			std::thread([jid, path] {
				zui_pixmap *p = zui_pixmap_load(path.c_str(), 192);
				app->post([jid, p] {
					Hold hold;
					if (p) {
						if (pictures[jid]) zui_pixmap_free(pictures[jid]);
						pictures[jid] = p;
						refresh_rows_of(jid);
					}
					pictures_next();
				});
			}).detach();
			return;
		}
		pictures_next();
	});
}
void avatar(Node row, const std::string &jid, const std::string &name) {
	if (zui_pixmap *p = picture_of(jid)) row.set_image(p);
	else row.set_monogram(first_letter(name));
	app->post([] { pictures_next(); });
}

/* ---- opening a chat ---- */
void show_chat(const std::string &id, const std::string &name_if_new = "");
void set_title_line();

void bind_chat(const std::vector<Chat> &v, int row, Node n) {
	if (size_t(row) >= v.size()) return;
	const Chat c = v[size_t(row)];
	std::string line = std::string(c.last_from_me ? "You: " : "") + one_line(c.last_text);
	Node it = n.item(c.name, line.empty() ? " " : line, [id = c.id] { show_chat(id); });
	avatar(it, c.id, c.name);
	it.set_detail(when_text(c.last_ts));
	it.set_emphasis(c.unread > 0);
	if (c.unread > 0) it.set_badge(c.unread > 999 ? "999+" : std::to_string(c.unread));
}

void chats_sort() {
	Hold hold;
	chats.clear();
	archived.clear();
	for (auto &[id, c] : all_chats) (c.archived ? archived : chats).push_back(c);
	auto order = [](const Chat &a, const Chat &b) {
		if (a.pinned != b.pinned) return a.pinned;
		return a.last_ts > b.last_ts;
	};
	std::stable_sort(chats.begin(), chats.end(), order);
	std::stable_sort(archived.begin(), archived.end(), order);
	if (list_chats) zui_list_set_count(list_chats.get(), int(chats.size()));
	if (list_archived) zui_list_set_count(list_archived.get(), int(archived.size()));
	if (note_chats) note_chats.set_visible(chats.empty());
}

void chat_from(const json &o) {
	Chat c;
	c.id = jstr(o, "id");
	if (c.id.empty()) return;
	c.name = jstr(o, "name");
	c.last_text = jstr(o, "last_text");
	c.is_group = jbool(o, "is_group");
	c.last_from_me = jbool(o, "last_from_me");
	c.muted = jbool(o, "muted");
	c.pinned = jbool(o, "pinned");
	c.archived = jbool(o, "archived");
	c.unread = int(jint(o, "unread"));
	c.last_ts = jint(o, "last_ts");
	all_chats[c.id] = std::move(c);
}

std::unique_ptr<Msg> msg_from(const json &o) {
	std::string kind = jstr(o, "kind");
	if (kind == "reaction" || kind == "hidden") return nullptr;
	auto m = std::make_unique<Msg>();
	m->id = jstr(o, "id");
	m->from_me = jbool(o, "from_me");
	m->sender_name = jstr(o, "sender_name");
	m->ts = jint(o, "timestamp");
	m->kind = kind;
	m->text = jstr(o, "text");
	m->status = jstr(o, "status");
	m->starred = jbool(o, "starred");
	m->system = kind == "system";
	if (m->text.empty() && kind != "text" && kind != "image") m->text = kind == "audio" ? "Voice message" : kind;
	auto media = o.find("media");
	m->quoted = jstr(o, "quoted_id");
	m->edited = jbool(o, "edited");
	if (kind == "sticker") {
		m->sticker = true;
		m->text.clear();
		if (media != o.end() && media->is_object()) m->path = jstr(*media, "path");
	}
	if (media != o.end() && media->is_object() && kind == "image") {
		m->image = true;
		m->w = int(jint(*media, "width"));
		m->h = int(jint(*media, "height"));
		m->thumb = jstr(*media, "thumb_b64");
		m->path = jstr(*media, "path");
	}
	return m;
}

std::string stamp_of(const Msg &m) {
	std::string t = (m.starred ? "* " : "") + std::string(m.edited ? "edited  " : "") + when_text(m.ts);
	if (!m.from_me) return t;
	if (m.status == "pending") t += "  sending";
	else if (m.status == "sent") t += "  sent";
	else if (m.status == "delivered") t += "  delivered";
	else if (m.status == "read") t += "  read";
	else if (m.status == "failed") t += "  not sent, tap to edit";
	return t;
}

/* a guess: the real height comes from laying the row out */
double guess_height(int row) {
	if (size_t(row) >= msgs.size()) return 60;
	const Msg &m = *msgs[size_t(row)];
	if (m.system) return 36;
	double lines = 1 + double(m.text.size()) / 32;
	if (m.sticker) return 170;
	return 52 + 22 * lines + (m.image ? 190 : 0);
}

void open_image(const std::string &id, const std::string &chat, const std::string &path);
void message_actions(const std::string &id);
zui_pixmap *sticker_pix(const Msg &m);

void bind_msg(int row, Node n) {
	if (size_t(row) >= msgs.size()) return;
	Msg &m = *msgs[size_t(row)];
	time_t a = m.ts, b = row > 0 ? msgs[size_t(row) - 1]->ts : 0;
	struct tm ta, tb;
	localtime_r(&a, &ta);
	localtime_r(&b, &tb);
	if (row == 0 ? !older_more : (ta.tm_yday != tb.tm_yday || ta.tm_year != tb.tm_year)) n.caption(day_text(m.ts)).set_align(2);
	if (m.system) {
		n.caption(m.text).set_align(2);
		n.space(6);
		return;
	}
	if (m.sticker) {
		/* as WhatsApp shows one: no bubble, its own shape on the page */
		zui_pixmap *sp = sticker_pix(m);
		Node pic = sp ? n.image(sp, 140, 140) : n.caption("Sticker");
		pic.set_align(m.from_me ? 1 : 0);
		pic.on([id = m.id](Node, int) { message_actions(id); });
		n.caption(stamp_of(m)).set_align(m.from_me ? 1 : 0);
		n.space(6);
		return;
	}
	Node bub = n.bubble(m.from_me);
	if (!m.quoted.empty()) {
		std::string said = "Reply";
		for (auto &q : msgs) if (q->id == m.quoted) { said = q->from_me ? "You: " : q->sender_name + ": "; said += one_line(q->text); break; }
		bub.caption(said);
	}
	if (!m.from_me && open_group && !m.sender_name.empty()) bub.caption(m.sender_name);
	if (m.image) {
		if (!m.pix && !m.thumb.empty()) {
			auto raw = unbase64(m.thumb);
			m.pix = zui_pixmap_decode(raw.data(), raw.size(), 0);
		}
		double aspect = m.w > 0 && m.h > 0 ? double(m.h) / m.w : 0.75;
		bub.image(m.pix, 240, std::clamp(240 * aspect, 90.0, 300.0)).set_radius(10);
	}
	if (!m.text.empty()) bub.text(m.text);
	bub.caption(stamp_of(m)).set_align(1);
	if (m.status == "failed") {
		bub.on([text = m.text](Node, int) {
			field_draft.set_text(text);
		});
	} else {
		bub.on([id = m.id](Node, int) { message_actions(id); });
	}
	n.space(6);
}

void refresh_rows_of(const std::string &jid) {
	for (size_t i = 0; i < chats.size(); i++) if (chats[i].id == jid) zui_list_refresh(list_chats.get(), int(i));
	for (size_t i = 0; i < archived.size(); i++) if (archived[i].id == jid) zui_list_refresh(list_archived.get(), int(i));
	for (size_t i = 0; i < statuses.size(); i++) if (statuses[i].sender == jid) zui_list_refresh(list_status.get(), int(i));
	for (size_t i = 0; i < starred.size(); i++) if (starred[i].chat == jid) zui_list_refresh(list_starred.get(), int(i));
	for (size_t i = 0; i < contacts_shown.size(); i++) if (contacts_shown[i].jid == jid && list_new) zui_list_refresh(list_new.get(), int(i));
}

void set_title_line() {
	std::string t = open_name;
	if (chat_page) { chat_page.set_text(t); chat_page.set_subtitle(presence_line.empty() ? "" : ", " + presence_line); }
}

/* While a chat is on screen and the app in front: PID CHAT, for the notifier. */
void focus_sync() {
	static std::string written;
	bool on = zui_current_page(app->get()) == chat_page.get() && zui_app_active(app->get()) && !open_id.empty();
	std::string want = on ? open_id : "";
	if (want == written) return;
	written = want;
	std::string path = run_file("ztwhatsapp.focus");
	if (want.empty()) { unlink(path.c_str()); return; }
	std::string temp = path + ".new";
	if (FILE *f = fopen(temp.c_str(), "w")) {
		fprintf(f, "%d %s\n", int(getpid()), want.c_str());
		fclose(f);
		rename(temp.c_str(), path.c_str());
	}
}
void focus_clear() { unlink(run_file("ztwhatsapp.focus").c_str()); }

void mark_open_read() {
	if (open_id.empty() || !zui_app_active(app->get())) return;
	auto it = all_chats.find(open_id);
	if (it != all_chats.end() && it->second.unread) request("mark_read", {{"chat", open_id}});
}

void show_chat(const std::string &id, const std::string &name_if_new) {
	if (opening) return;
	opening = true;
	auto it = all_chats.find(id);
	open_id = id;
	open_name = it != all_chats.end() ? it->second.name : name_if_new;
	open_group = it != all_chats.end() && it->second.is_group;
	read_only = false;
	reply_id.clear();
	edit_id.clear();
	avatar(chat_page, open_id, open_name);
	presence_line.clear();
	older_more = older_pending = false;
	request("messages", {{"chat", id}, {"limit", PAGE_SIZE}}, [id](const json *r, const char *err) {
		opening = false;
		if (id != open_id) return;
		Hold hold;
		msgs.clear();
		if (!err) {
			if (r->contains("messages")) for (auto &o : (*r)["messages"]) if (auto m = msg_from(o)) msgs.push_back(std::move(m));
			older_more = jbool(*r, "more");
		}
		zui_list_set_count(list_msgs.get(), int(msgs.size()));
		set_title_line();
		field_draft.set_text("");
		chat_page.push();
		zui_page_scroll_end(chat_page.get(), false);
		request("subscribe_presence", {{"chat", id}});
		mark_open_read();
		focus_sync();
	});
}

void open_pending() {
	if (pending_open.empty() || sock_fd < 0) return;
	std::string id = pending_open;
	pending_open.clear();
	if (id != open_id || zui_current_page(app->get()) != chat_page.get()) show_chat(id);
}

void load_older() {
	if (older_pending || !older_more || open_id.empty() || msgs.empty()) return;
	older_pending = true;
	std::string id = open_id;
	request("messages", {{"chat", id}, {"limit", PAGE_SIZE}, {"before_ts", msgs.front()->ts}, {"before_id", msgs.front()->id}},
		[id](const json *r, const char *err) {
			older_pending = false;
			if (id != open_id || err || !r->contains("messages")) return;
			Hold hold;
			int added = 0;
			auto &list = (*r)["messages"];
			for (auto it = list.rbegin(); it != list.rend(); ++it)
				if (auto m = msg_from(*it)) { msgs.push_front(std::move(m)); added++; }
			older_more = jbool(*r, "more");
			if (added) zui_list_prepend(list_msgs.get(), added);
			if (older_more) zui_list_allow_more(list_msgs.get());
		});
}

void load_chats() {
	request("chats", {{"limit", 500}}, [](const json *r, const char *err) {
		if (err || !r->contains("chats")) return;
		all_chats.clear();
		for (auto &o : (*r)["chats"]) chat_from(o);
		chats_sort();
		open_pending();
	});
}
Found found_from(const json &o) {
	Found f;
	f.chat = jstr(o, "chat");
	auto it = all_chats.find(f.chat);
	f.name = it != all_chats.end() ? it->second.name : jstr(o, "sender_name");
	f.text = jstr(o, "text");
	if (f.text.empty()) f.text = jstr(o, "kind");
	f.ts = jint(o, "timestamp");
	return f;
}
void load_statuses() {
	request("statuses", json::object(), [](const json *r, const char *err) {
		if (err || !r->contains("contacts")) return;
		Hold hold;
		statuses.clear();
		for (auto &c : (*r)["contacts"]) {
			StatusPerson p;
			p.sender = jstr(c, "sender");
			p.name = jstr(c, "name");
			p.last_ts = jint(c, "last_ts");
			p.unseen = int(jint(c, "unseen"));
			if (c.contains("items")) for (auto &it : c["items"]) {
				StatusItem si;
				si.id = jstr(it, "id");
				si.text = jstr(it, "text");
				si.kind = jstr(it, "kind");
				si.ts = jint(it, "timestamp");
				si.seen = jbool(it, "seen");
				auto media = it.find("media");
				if (media != it.end() && media->is_object()) { si.thumb = jstr(*media, "thumb_b64"); si.path = jstr(*media, "path"); }
				p.items.push_back(std::move(si));
			}
			statuses.push_back(std::move(p));
		}
		zui_list_set_count(list_status.get(), int(statuses.size()));
	});
}
void load_starred() {
	request("starred", {{"limit", 200}}, [](const json *r, const char *err) {
		if (err || !r->contains("messages")) return;
		Hold hold;
		starred.clear();
		for (auto &o : (*r)["messages"]) starred.push_back(found_from(o));
		zui_list_set_count(list_starred.get(), int(starred.size()));
	});
}
void contacts_filter() {
	Hold hold;
	std::string q = new_text;
	std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return char(tolower(c)); });
	contacts_shown.clear();
	for (auto &c : contacts) {
		std::string n = c.name;
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char x) { return char(tolower(x)); });
		if (q.empty() || n.find(q) != std::string::npos || c.phone.find(q) != std::string::npos) contacts_shown.push_back(c);
	}
	if (list_new) zui_list_set_count(list_new.get(), int(contacts_shown.size()));
	if (new_note) new_note.set_visible(contacts_shown.empty());
}
void load_contacts() {
	request("contacts", {{"limit", 5000}}, [](const json *r, const char *err) {
		if (err || !r->contains("contacts")) return;
		contacts.clear();
		for (auto &c : (*r)["contacts"]) contacts.push_back({jstr(c, "jid"), jstr(c, "name"), jstr(c, "phone")});
		contacts_filter();
	});
}

/* ---- pairing ---- */
void build_link() {
	link_box.clear();
	if (pair_state == "qr") {
		link_box.text("On your phone, open WhatsApp, then Linked devices, then Link a device, and scan this.");
		qr::Code c = qr::encode(pair_qr);
		if (c.n) {
			const int quiet = 4, side = c.n + 2 * quiet;
			std::vector<uint8_t> px(size_t(side) * side * 4, 255);
			for (int y = 0; y < c.n; y++) for (int x = 0; x < c.n; x++) if (c.at(x, y)) {
				uint8_t *o = &px[(size_t(y + quiet) * side + size_t(x + quiet)) * 4];
				o[0] = o[1] = o[2] = 0;
			}
			zui_pixmap *pix = zui_pixmap_rgba(side, side, px.data());
			/* a whole number of pixels for each module (4 pixels a point on the S9) */
			double want = 324;
			const char *sc = getenv("ZTMETRO_SCALE");
			double scale = sc ? atof(sc) : 2;
			if (scale < 1) scale = 1;
			double each = std::floor(want * scale / side);
			link_box.image(pix, each * side / scale, each * side / scale).set_image_sharp(true);
			zui_pixmap_free(pix);
		}
		link_box.button("Cancel", [] { request("pair_cancel"); }).set_variant(Variant::Tonal);
	} else if (pair_state == "phone_code") {
		link_box.text("On your phone, open WhatsApp, then Linked devices, then Link a device, then Link with phone number, and type this code.");
		link_box.heading(pair_code);
		link_box.button("Cancel", [] { request("pair_cancel"); }).set_variant(Variant::Tonal);
	} else if (pair_state == "pairing") {
		link_box.text("Finishing the link...");
	} else if (pair_state == "unpaired") {
		if (!pair_error.empty()) link_box.caption(pair_error);
		link_box.text("This is an unofficial WhatsApp client. WhatsApp's terms do not allow it, and a number can be banned for using one. Link it only if you accept that.");
		link_box.button("Show a QR code", [] { pair_error.clear(); request("pair_qr"); });
		link_box.field("Your number, with country code", [](const std::string &t, bool) { phone_text = t; });
		link_box.button("Use phone number", [] {
			std::string d;
			for (char c : phone_text) if (isdigit(static_cast<unsigned char>(c))) d += c;
			if (d.size() < 8) { app->toast("Type your whole number, with its country code"); return; }
			request("pair_phone", {{"phone", d}}, [](const json *, const char *err) { if (err) app->toast("That did not work"); });
		}).set_variant(Variant::Tonal);
	} else {
		link_box.text(sock_fd < 0 ? "Waiting for the WhatsApp service..." : "Connecting...");
	}
}

/* ---- status ---- */
void show_status_item() {
	status_box.clear();
	if (status_who < 0 || size_t(status_who) >= statuses.size()) return;
	StatusPerson &p = statuses[size_t(status_who)];
	if (size_t(status_at) >= p.items.size()) return;
	StatusItem &it = p.items[size_t(status_at)];
	status_page.set_text(p.name);
	status_box.caption(std::to_string(status_at + 1) + " of " + std::to_string(p.items.size()) + ", " + when_text(it.ts));
	if (it.kind == "image") {
		zui_pixmap *pix = nullptr;
		if (!it.thumb.empty()) { auto raw = unbase64(it.thumb); pix = zui_pixmap_decode(raw.data(), raw.size(), 0); }
		Node im = status_box.image(pix, 0, 0).set_radius(12);
		if (pix) zui_pixmap_free(pix);
		auto load = [im](const std::string &path) mutable {
			std::thread([im, path]() mutable {
				zui_pixmap *full = zui_pixmap_load(path.c_str(), 1440);
				app->post([im, full]() mutable {
					if (full) { im.set_image(full); zui_pixmap_free(full); }
				});
			}).detach();
		};
		if (!it.path.empty()) load(it.path);
		else request("download_media", {{"chat", "status@broadcast"}, {"message_id", it.id}}, [load](const json *r, const char *err) mutable {
			std::string path = err ? "" : jstr(*r, "path");
			if (path.empty()) app->toast("Could not download this update");
			else load(path);
		});
	} else if (it.kind == "video") {
		status_box.text("Video updates can only be seen on your phone.");
	}
	if (!it.text.empty()) status_box.heading(it.text);
	status_box.button(size_t(status_at + 1) < p.items.size() ? "Next" : "Done", [] {
		StatusPerson &q = statuses[size_t(status_who)];
		if (size_t(status_at + 1) < q.items.size()) { status_at++; show_status_item(); }
		else app->back();
	}).set_variant(Variant::Tonal);
	if (!it.seen) {
		Hold hold;
		it.seen = true;
		if (p.unseen > 0) p.unseen--;
		request("status_seen", {{"sender", p.sender}, {"ids", json::array({it.id})}});
		zui_list_refresh(list_status.get(), status_who);
	}
}

/* ---- photo ---- */
void show_image_path(const std::string &path) {
	std::thread([path] {
		zui_pixmap *full = zui_pixmap_load(path.c_str(), 2048);
		app->post([full] {
			image_box.clear();
			if (!full) { image_box.text("Could not open this photo."); return; }
			image_box.image(full, 0, 0).set_radius(8);
			zui_pixmap_free(full);
		});
	}).detach();
}
void open_image(const std::string &id, const std::string &chat, const std::string &path) {
	image_box.clear();
	image_box.caption("Opening...");
	image_page.push();
	if (!path.empty()) { show_image_path(path); return; }
	request("download_media", {{"chat", chat}, {"message_id", id}}, [id](const json *r, const char *err) {
		std::string p = err ? "" : jstr(*r, "path");
		if (p.empty()) {
			image_box.clear();
			image_box.text("Could not download this photo.");
			return;
		}
		for (auto &m : msgs) if (m->id == id) m->path = p;
		show_image_path(p);
	});
}

/* ---- info ---- */
/* ---- what can be done with one message ---- */
void message_actions(const std::string &id) {
	Msg *m = nullptr;
	for (auto &q : msgs) if (q->id == id) m = q.get();
	if (!m) return;
	message_box.clear();
	Node c = message_box.column();
	bool mine = m->from_me, text = m->kind == "text" && !m->text.empty(), image = m->image, sticker = m->sticker;
	std::string body = m->text, path = m->path;
	if (image) c.button("Open", [id, path] { app->back(); open_image(id, open_id, path); });
	if (read_only) { c.caption("A channel's posts can only be read."); message_page.push(); return; }
	c.button("Reply", [id] { reply_id = id; edit_id.clear(); app->back(); app->toast("Replying: type it below"); });
	c.space(8);
	if (mine && text) {
		c.button("Edit", [id, body] { edit_id = id; reply_id.clear(); field_draft.set_text(body); app->back(); });
		c.space(8);
	}
	if (sticker) {
		c.button("Save to favourites", [id] { save_sticker(id); app->back(); });
		c.space(8);
	}
	c.button("Delete for me", [id] {
		request("delete_message", {{"chat", open_id}, {"id", id}}, [id](const json *, const char *err) {
			if (err) return app->toast("Not deleted");
			Hold hold;
			msgs.erase(std::remove_if(msgs.begin(), msgs.end(), [&](auto &q) { return q->id == id; }), msgs.end());
			zui_list_set_count(list_msgs.get(), int(msgs.size()));
		});
		app->back();
	});
	if (mine) {
		c.space(8);
		c.button("Delete for everyone", [id] {
			request("delete_message", {{"chat", open_id}, {"id", id}, {"for_everyone", true}}, [id](const json *, const char *err) {
				if (err) return app->toast("Not deleted: are you online?");
				Hold hold;
				for (auto &q : msgs) if (q->id == id) { q->system = true; q->text = "You deleted this message"; }
				zui_list_set_count(list_msgs.get(), int(msgs.size()));
			});
			app->back();
		}).set_variant(Variant::Danger);
	}
	message_page.push();
}

void do_send() {
	std::string text = field_draft.text_value();
	if (open_id.empty() || text.empty()) return;
	if (read_only) return app->toast("A channel's posts can only be read");
	field_draft.set_text("");
	zui_haptic("click");
	request("typing", {{"chat", open_id}, {"state", "paused"}});
	last_typing = 0;
	if (!edit_id.empty()) {
		std::string id = edit_id;
		edit_id.clear();
		request("edit_message", {{"chat", open_id}, {"id", id}, {"text", text}}, [id, text](const json *, const char *err) {
			if (err) return app->toast("Not edited: are you online?");
			Hold hold;
			for (auto &q : msgs) if (q->id == id) { q->text = text; q->edited = true; }
			zui_list_set_count(list_msgs.get(), int(msgs.size()));
		});
		return;
	}
	json req = {{"chat", open_id}, {"text", text}};
	if (!reply_id.empty()) req["quoted_id"] = reply_id;
	reply_id.clear();
	request("send_text", req, [text](const json *, const char *err) {
		if (err) {
			if (field_draft.text_value().empty()) field_draft.set_text(text);
			app->toast("Not sent. Your text is back in the box");
		}
	});
}

/* ---- stickers: read off the UI thread, each once, the first frame only: nothing runs on a timer ---- */
zui_pixmap *decode_webp(const std::string &path) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return nullptr;
	std::vector<uint8_t> raw;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0 && raw.size() < 4u << 20) raw.insert(raw.end(), buf, buf + n);
	fclose(f);
	WebPAnimDecoderOptions o;
	WebPAnimDecoderOptionsInit(&o);
	o.color_mode = MODE_rgbA;
	o.use_threads = 0;
	WebPData d = {raw.data(), raw.size()};
	WebPAnimDecoder *dec = WebPAnimDecoderNew(&d, &o);
	if (!dec) return nullptr;
	WebPAnimInfo info;
	uint8_t *px = nullptr;
	int ts = 0;
	zui_pixmap *p = nullptr;
	if (WebPAnimDecoderGetInfo(dec, &info) && WebPAnimDecoderGetNext(dec, &px, &ts) && px) p = zui_pixmap_rgba(int(info.canvas_width), int(info.canvas_height), px);
	WebPAnimDecoderDelete(dec);
	return p;
}
std::string stickers_dir() {
	std::string d = std::string(getenv("HOME") ? getenv("HOME") : "") + "/.local/share/zrn-whatsapp";
	mkdir(d.c_str(), 0700);
	d += "/stickers";
	mkdir(d.c_str(), 0700);
	return d;
}
void refresh_msg(const std::string &id) {
	for (size_t i = 0; i < msgs.size(); i++) if (msgs[i]->id == id) zui_list_refresh(list_msgs.get(), int(i));
}
void sticker_load(const std::string &id, const std::string &path) {
	std::thread([id, path] {
		zui_pixmap *p = decode_webp(path);
		app->post([id, p] {
			Hold hold;
			if (!p) return;
			sticker_cache[id] = p;
			refresh_msg(id);
		});
	}).detach();
}
zui_pixmap *sticker_pix(const Msg &m) {
	auto it = sticker_cache.find(m.id);
	if (it != sticker_cache.end()) return it->second;
	sticker_cache[m.id] = nullptr;	/* asked for once */
	if (!m.path.empty()) { sticker_load(m.id, m.path); return nullptr; }
	if (!pref_stickers) return nullptr;
	std::string id = m.id;
	request("download_media", {{"chat", open_id}, {"message_id", id}}, [id](const json *r, const char *err) {
		std::string path = err ? "" : jstr(*r, "path");
		if (path.empty()) return;
		Hold hold;
		for (auto &q : msgs) if (q->id == id) q->path = path;
		sticker_load(id, path);
	});
	return nullptr;
}
void save_sticker(const std::string &id) {
	std::string path;
	for (auto &q : msgs) if (q->id == id) path = q->path;
	if (path.empty()) return app->toast("Wait until it has loaded");
	std::string to = stickers_dir() + "/" + id + ".webp";
	FILE *a = fopen(path.c_str(), "rb"), *b = a ? fopen(to.c_str(), "wb") : nullptr;
	uint8_t buf[65536];
	size_t n;
	while (a && b && (n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
	if (a) fclose(a);
	if (b) fclose(b);
	app->toast(b ? "Saved to favourites" : "Not saved");
}
void show_stickers() {
	stickers_box.clear();
	Node c = stickers_box.column();
	DIR *d = opendir(stickers_dir().c_str());
	int shown = 0;
	while (dirent *e = d ? readdir(d) : nullptr) {
		std::string name = e->d_name;
		if (name.size() < 6 || name.substr(name.size() - 5) != ".webp") continue;
		std::string path = stickers_dir() + "/" + name;
		zui_pixmap *p = decode_webp(path);
		if (!p) continue;
		shown++;
		c.image(p, 110, 110).on([path](Node, int) {
			request("send_sticker", {{"chat", open_id}, {"path", path}}, [](const json *, const char *err) {
				if (err) app->toast("Not sent: are you online?");
			});
			app->back();
		});
		zui_pixmap_free(p);
		c.space(8);
	}
	if (d) closedir(d);
	if (!shown) c.caption("None yet. Tap a sticker in a chat to save it here.");
	stickers_page.push();
}

/* ---- channels ---- */
void load_channels() {
	request("channels", {}, [](const json *r, const char *err) {
		Hold hold;
		channels.clear();
		if (!err && r->contains("channels")) for (auto &o : (*r)["channels"]) channels.push_back({jstr(o, "id"), jstr(o, "name"), jstr(o, "description")});
		zui_list_set_count(list_channels.get(), int(channels.size()));
		note_channels.set_text(err ? "Could not load channels." : channels.empty() ? "Not following any yet." : "");
	});
}
void open_channel(const std::string &id, const std::string &name) {
	open_id = id;
	open_name = name;
	open_group = false;
	read_only = true;
	presence_line.clear();
	older_more = false;
	avatar(chat_page, id, name);
	request("channel_messages", {{"id", id}, {"limit", 50}}, [id](const json *r, const char *err) {
		if (id != open_id) return;
		Hold hold;
		msgs.clear();
		if (err) return app->toast("Could not load this channel");
		for (auto &o : (*r)["messages"]) if (auto m = msg_from(o)) msgs.push_back(std::move(m));
		zui_list_set_count(list_msgs.get(), int(msgs.size()));
		set_title_line();
		chat_page.push();
		zui_page_scroll_end(chat_page.get(), false);
	});
}

/* ---- settings: what each does is real, and kept in ~/.config/zrn-whatsapp/prefs ---- */
void prefs_save() {
	mkdir((std::string(getenv("HOME") ? getenv("HOME") : "") + "/.config/zrn-whatsapp").c_str(), 0700);
	if (FILE *f = fopen(prefs_path().c_str(), "w")) {
		fprintf(f, "follow_system_accent=%d\nenter_sends=%d\nload_stickers=%d\nnotify=%d\nnotify_preview=%d\n",
			follow_accent, pref_enter, pref_stickers, pref_notify, pref_preview);
		fclose(f);
	}
}
void show_settings() {
	settings_box.clear();
	Node c = settings_box.card();
	c.toggle("Notifications", pref_notify, [](bool on) { pref_notify = on; prefs_save(); });
	c.toggle("Show message text in notifications", pref_preview, [](bool on) { pref_preview = on; prefs_save(); });
	c.toggle("Enter sends", pref_enter, [](bool on) { pref_enter = on; prefs_save(); });
	c.toggle("Load stickers by themselves", pref_stickers, [](bool on) { pref_stickers = on; prefs_save(); });
	c.toggle("Use the system accent colour", follow_accent, [](bool on) {
		follow_accent = on;
		prefs_save();
		app->toast("Applies the next time the app opens");
	});
	settings_page.push();
}

void show_info() {
	info_box.clear();
	info_page.set_text(open_name);
	Node card = info_box.card();
	Node who = card.item(open_name, open_group ? "Group" : "");
	avatar(who, open_id, open_name);
	size_t at = open_id.find("@s.whatsapp.net");
	if (!open_group && at != std::string::npos) card.item("Number", "+" + open_id.substr(0, at));
	auto it = all_chats.find(open_id);
	if (it != all_chats.end()) {
		card.item("Notifications", it->second.muted ? "Muted" : "On");
		card.toggle("Pin to the top", it->second.pinned, [id = open_id](bool on) {
			request("pin_chat", {{"chat", id}, {"pinned", on}}, [](const json *, const char *err) {
				if (err) return app->toast("Not pinned");
				load_chats();
			});
		});
		if (it->second.archived) card.item("Archived");
	}
	info_box.caption("A pin stays on this phone. Mute and archive are changed on your phone.");
	info_page.push();
	if (!open_group) return;
	std::string id = open_id;
	request("group_info", {{"chat", id}}, [id](const json *r, const char *err) {
		if (id != open_id) return;
		if (err) { info_box.caption("Could not get the group's details."); return; }
		std::string topic = jstr(*r, "topic");
		if (!topic.empty()) info_box.text(topic);
		if (!r->contains("participants")) return;
		auto &ps = (*r)["participants"];
		Node c = info_box.section(std::to_string(ps.size()) + " participants");
		int shown = 0;
		for (auto &m : ps) {
			if (shown++ >= 100) break;
			Node row = c.item(jstr(m, "name"));
			if (jbool(m, "is_admin") || jbool(m, "is_super_admin")) row.set_detail("Admin");
		}
	});
}

/* ---- search ---- */
void build_search() {
	search_box.clear();
	if (search_text.empty()) return;
	std::string q = search_text;
	std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return char(tolower(c)); });
	Node card;
	int shown = 0;
	auto consider = [&](const Chat &c) {
		std::string n = c.name;
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char x) { return char(tolower(x)); });
		if (n.find(q) == std::string::npos || shown >= 8) return;
		if (!card) card = search_box.section("Chats");
		card.item(c.name, one_line(c.last_text), [id = c.id] { show_chat(id); });
		shown++;
	};
	for (auto &c : chats) consider(c);
	for (auto &c : archived) consider(c);
	if (!found.empty()) {
		Node m = search_box.section("Messages");
		for (size_t i = 0; i < found.size() && i < 40; i++)
			m.item(found[i].name, one_line(found[i].text), [id = found[i].chat] { show_chat(id); }).set_detail(when_text(found[i].ts));
	}
}

/* ---- events ---- */
void on_event(const json &o) {
	std::string ev = jstr(o, "event");
	if (ev == "hello") {
		foreground = false;
	} else if (ev == "connection") {
		std::string was = connection;
		connection = jstr(o, "state");
		if (connection == "connected" && was != "connected") { load_chats(); load_statuses(); load_starred(); }
	} else if (ev == "pairing") {
		std::string st = jstr(o, "state");
		pair_error = jstr(o, "error");
		if (st == "failed") st = "unpaired";
		if (st == "qr") pair_qr = jstr(o, "qr");
		if (st == "phone_code") pair_code = jstr(o, "code");
		pair_state = st;
		if (st == "paired") {
			self_jid = jstr(o, "self");
			load_chats();
			if (zui_current_page(app->get()) == link_page.get()) app->back();
		} else {
			build_link();
			if (zui_current_page(app->get()) != link_page.get()) link_page.push();
		}
	} else if (ev == "chat") {
		if (o.contains("chat")) chat_from(o["chat"]);
		static bool queued;
		if (!queued) {
			queued = true;	/* a burst of them is one rebuild */
			app->post([] { queued = false; chats_sort(); });
		}
	} else if (ev == "message") {
		if (jstr(o, "chat") != open_id || !o.contains("message")) return;
		auto m = msg_from(o["message"]);
		if (!m) return;
		Hold hold;
		bool end = m->from_me || zui_page_at_end(chat_page.get(), 120);
		int known = -1;
		for (int i = int(msgs.size()) - 1; i >= 0 && known < 0; i--) if (msgs[size_t(i)]->id == m->id) known = i;
		if (known >= 0) {
			msgs[size_t(known)] = std::move(m);
			zui_list_refresh(list_msgs.get(), known);
		} else {
			msgs.push_back(std::move(m));
			zui_list_append(list_msgs.get(), 1);
			if (end) zui_page_scroll_end(chat_page.get(), true);
		}
		if (zui_app_active(app->get()) && zui_current_page(app->get()) == chat_page.get()) request("mark_read", {{"chat", open_id}});
	} else if (ev == "receipt") {
		if (jstr(o, "chat") != open_id || !o.contains("ids")) return;
		std::string st = jstr(o, "status");
		Hold hold;
		for (auto &idv : o["ids"]) {
			if (!idv.is_string()) continue;
			for (int i = int(msgs.size()) - 1; i >= 0; i--) if (msgs[size_t(i)]->id == idv.get<std::string>()) {
				msgs[size_t(i)]->status = st;
				zui_list_refresh(list_msgs.get(), i);
				break;
			}
		}
	} else if (ev == "presence") {
		if (jstr(o, "chat") != open_id) return;
		if (jbool(o, "typing")) presence_line = "typing";
		else if (jbool(o, "available")) presence_line = "online";
		else presence_line.clear();
		set_title_line();
	} else if (ev == "history_sync" || ev == "app_state") {
		if (jstr(o, "state") == "done") { load_chats(); load_statuses(); load_starred(); }
	} else if (ev == "status") {
		load_statuses();
	} else if (ev == "contacts") {
		if (zui_current_page(app->get()) == new_page.get()) load_contacts();
	} else if (ev == "picture") {
		std::string jid = jstr(o, "jid");
		Hold hold;
		pictures.erase(jid);
		picture_of(jid);
		pictures_next();
	}
}

void disconnect_daemon();
void sock_ready(int fd, void *) {
	char chunk[65536];
	bool gone = false;
	for (;;) {
		ssize_t n = recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
		if (n > 0) { in_buf.append(chunk, size_t(n)); continue; }
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) gone = true;
		break;
	}
	size_t start = 0, nl;
	while ((nl = in_buf.find('\n', start)) != std::string::npos) {
		json o = json::parse(in_buf.begin() + long(start), in_buf.begin() + long(nl), nullptr, false);
		start = nl + 1;
		if (o.is_discarded() || !o.is_object()) continue;
		/* (a message event has an "id" too: the message's, a string) */
		if (o.contains("event")) {
			on_event(o);
		} else if (o.contains("id") && o["id"].is_number()) {
			auto it = waiting.find(o["id"].get<int>());
			if (it != waiting.end()) {
				Reply r = std::move(it->second);
				waiting.erase(it);
				if (jbool(o, "ok")) {
					json empty = json::object();
					r(o.contains("result") ? &o["result"] : &empty, nullptr);
				} else {
					std::string e = jstr(o, "error");
					r(nullptr, e.empty() ? "internal" : e.c_str());
				}
			}
		}
	}
	in_buf.erase(0, start);
	flush_out();
	if (gone) disconnect_daemon();
}

std::string sock_path() {
	if (const char *o = getenv("ZTWHATSAPP_SOCK")) return o;
	return run_file("ztwhatsapp.sock");
}

bool connect_daemon() {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un a {};
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path().c_str());
	if (fd < 0 || connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) {
		if (fd >= 0) close(fd);
		return false;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	sock_fd = fd;
	zui_watch_fd(app->get(), fd, sock_ready, nullptr);
	return true;
}

/* Detached: they stay when the window closes, so notifications keep coming.
 * They do not start by themselves after a reboot: see the README. */
void spawn_role(const char *role) {
	char self[1024];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n <= 0) return;
	self[n] = 0;
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			int null = open("/dev/null", O_RDWR);
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
			for (int fd = 3; fd < 256; fd++) close(fd);
			execl(self, self, role, nullptr);
		}
		_exit(0);
	}
	if (pid > 0) waitpid(pid, nullptr, 0);
}
void start_services() {
	if (services_started || getenv("ZTWHATSAPP_NO_SPAWN")) return;
	services_started = true;
	spawn_role("--daemon");
}

/* After an upgrade the engine and notifier of the old version are still running, and
 * the engine's lock keeps the new one out. Anything of ours from another binary goes. */
void replace_old_roles() {
	if (getenv("ZTWHATSAPP_NO_SPAWN")) return;
	char self[1024];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n <= 0) return;
	self[n] = 0;
	std::vector<pid_t> old;
	DIR *d = opendir("/proc");
	while (dirent *e = d ? readdir(d) : nullptr) {
		pid_t pid = atoi(e->d_name);
		if (pid <= 0 || pid == getpid()) continue;
		char path[64], exe[1024];
		snprintf(path, sizeof(path), "/proc/%d/exe", int(pid));
		ssize_t k = readlink(path, exe, sizeof(exe) - 1);
		if (k <= 0) continue;
		exe[k] = 0;
		std::string name = exe;
		if (name.find("/bin/zrn-whatsapp") == std::string::npos || name == self) continue;
		old.push_back(pid);
	}
	if (d) closedir(d);
	for (pid_t pid : old) kill(pid, SIGTERM);
	for (int i = 0; i < 20 && !old.empty(); i++) {
		usleep(100000);
		old.erase(std::remove_if(old.begin(), old.end(), [](pid_t p) { return kill(p, 0) != 0; }), old.end());
	}
	for (pid_t pid : old) kill(pid, SIGKILL);
}

void disconnect_daemon() {
	if (sock_fd >= 0) {
		zui_watch_fd(app->get(), sock_fd, nullptr, nullptr);
		close(sock_fd);
	}
	sock_fd = -1;
	in_buf.clear();
	out_buf.clear();
	auto pending = std::move(waiting);
	waiting.clear();
	for (auto &[id, r] : pending) r(nullptr, "not_connected");
	connection = "connecting";
	foreground = false;
}

void tick() {
	if (sock_fd < 0) {
		if (!connect_daemon()) start_services();
		return;
	}
	focus_sync();
	bool active = zui_app_active(app->get());
	if (active != foreground) {
		foreground = active;
		request("set_presence", {{"available", active}});
		if (active && zui_current_page(app->get()) == chat_page.get()) mark_open_read();
	}
}

/* ---- the notifier asks for a chat ---- */
void open_file_check() {
	std::string path = run_file("ztwhatsapp.open");
	FILE *f = fopen(path.c_str(), "r");
	if (!f) return;
	char line[512] = "";
	char *got = fgets(line, sizeof(line), f);
	fclose(f);
	unlink(path.c_str());
	if (!got) return;
	line[strcspn(line, " \r\n")] = 0;
	if (line[0]) {
		pending_open = line;
		if (!all_chats.empty()) open_pending();
	}
}
void run_dir_event(int fd, void *) {
	char drain[1024];
	while (read(fd, drain, sizeof(drain)) > 0) {}
	open_file_check();
}

std::string prefs_path() {
	const char *home = getenv("HOME");
	return std::string(home ? home : "") + "/.config/zrn-whatsapp/prefs";
}
void accent_apply() {
	if (follow_accent) return;	/* libzrnui reads the system's by itself; set at start */
	app->set_accent(WA_GREEN);
}

/* ---- the pages ---- */
void build_pages(uint32_t system_accent) {
	main_page = app->page("WhatsApp");
	tabs = main_page.tabs();
	Node t_chats = tabs.tab("chats"), t_status = tabs.tab("status"), t_star = tabs.tab("starred"), t_arch = tabs.tab("archived"), t_chan = tabs.tab("channels");
	Node actions = main_page.footer().row();
	actions.set_bar(true);
	actions.column().set_flex(1);
	actions.button("search", [] { search_page.push(); }).set_icon("search");
	actions.button("new", [] {
		new_text.clear();
		contacts_filter();
		load_contacts();
		new_page.push();
	}).set_icon("plus");
	actions.button("settings", [] { show_settings(); }).set_icon("settings");
	actions.button("account", [] { 
		account_box.clear();
		logout_ask = false;
		Node c = account_box.card();
		c.item("Connection", connection);
		account_box.caption("This is an unofficial client. WhatsApp's terms do not allow it, and a number can be banned for using one.");
		account_box.button("Unlink this phone", [] {
			if (!logout_ask) { logout_ask = true; app->toast("Tap again: this unlinks and deletes the messages stored here"); return; }
			logout_ask = false;
			request("logout");
		}).set_variant(Variant::Danger);
		account_page.push();
	}).set_icon("person");
	actions.column().set_flex(1);
	note_chats = t_chats.caption("No chats yet.");
	note_chats.set_visible(false);
	list_chats = t_chats.list(0, [](int) { return 72.0; }, [](int r, Node n) { bind_chat(chats, r, n); });
	t_status.button("new status", [] { post_note.clear(); post_note_n.set_text(""); post_page.push(); }).set_variant(Variant::Tonal);
	list_status = t_status.list(0, [](int) { return 72.0; }, [](int r, Node n) {
		if (size_t(r) >= statuses.size()) return;
		const StatusPerson p = statuses[size_t(r)];
		Node it = n.item(p.name, p.unseen ? std::to_string(p.unseen) + " new" : "Seen", [r] {
			status_who = r;
			status_at = 0;
			auto &items = statuses[size_t(r)].items;
			for (size_t i = 0; i < items.size(); i++) if (!items[i].seen) { status_at = int(i); break; }
			show_status_item();
			status_page.push();
		});
		avatar(it, p.sender, p.name);
		it.set_detail(when_text(p.last_ts));
		it.set_emphasis(p.unseen > 0);
	});
	list_starred = t_star.list(0, [](int) { return 72.0; }, [](int r, Node n) {
		if (size_t(r) >= starred.size()) return;
		const Found f = starred[size_t(r)];
		Node it = n.item(f.name, one_line(f.text), [id = f.chat] { show_chat(id); });
		avatar(it, f.chat, f.name);
		it.set_detail(when_text(f.ts));
	});
	list_archived = t_arch.list(0, [](int) { return 72.0; }, [](int r, Node n) { bind_chat(archived, r, n); });
	zui_set_handler(tabs.get(), [](zui_node *, void *) {
		int i = zui_tabs_current(tabs.get());
		if (i == 1) load_statuses();
		if (i == 2) load_starred();
		if (i == 4) load_channels();
	}, nullptr, nullptr);

	note_channels = t_chan.caption("");
	t_chan.button("follow a channel", [] { follow_field.set_text(""); follow_page.push(); }).set_variant(Variant::Primary);
	list_channels = t_chan.list(0, [](int) { return 72.0; }, [](int r, Node n) {
		if (size_t(r) >= channels.size()) return;
		Channel c = channels[size_t(r)];
		Node it = n.item(c.name, one_line(c.about), [c] { open_channel(c.id, c.name); });
		avatar(it, c.id, c.name);
	});
	follow_page = app->page("Follow a channel");
	follow_field = follow_page.field("Its invite link or code", [](const std::string &, bool) {});
	follow_page.button("Follow", [] {
		request("channel_follow", {{"code", follow_field.text_value()}}, [](const json *, const char *err) {
			if (err) return app->toast("Could not follow that");
			app->back();
			load_channels();
		});
	});
	message_page = app->page("Message");
	message_box = message_page.column();
	stickers_page = app->page("Stickers");
	stickers_box = stickers_page.column();
	settings_page = app->page("Settings");
	settings_box = settings_page.column();

	chat_page = app->page("");
	list_msgs = chat_page.list(0, guess_height, bind_msg);
	zui_set_handler(list_msgs.get(), [](zui_node *, void *) { load_older(); }, nullptr, nullptr);
	chat_page.set_compact(true);
	Node footer = chat_page.footer();
	field_draft = footer.field("type a message", [](const std::string &t, bool submit) {
		if (submit) { if (pref_enter) do_send(); return; }
		long now = time(nullptr);
		if (!t.empty() && now - last_typing >= 5 && !open_id.empty()) {
			last_typing = now;
			request("typing", {{"chat", open_id}, {"state", "composing"}});
		}
	});
	field_draft.set_multiline(true);
	footer.space(16);
	Node foot = footer.row();
	foot.set_bar(true);
	foot.column().set_flex(1);
	foot.button("stickers", [] { show_stickers(); }).set_icon("smile");
	foot.button("send", [] { do_send(); }).set_icon("send");
	foot.button("info", [] { show_info(); }).set_icon("more");
	foot.column().set_flex(1);

	link_page = app->page("Link device");
	link_box = link_page.column();
	search_page = app->page("Search");
	search_page.field("Chats and messages", [](const std::string &t, bool submit) {
		search_text = t;
		if (submit && !t.empty()) {
			app->busy(true);
			request("search", {{"query", t}, {"limit", 40}}, [](const json *r, const char *err) {
				app->busy(false);
				found.clear();
				if (!err && r->contains("messages")) for (auto &o : (*r)["messages"]) found.push_back(found_from(o));
				build_search();
			});
			return;
		}
		found.clear();
		build_search();
	});
	search_page.caption("Enter searches the messages too.");
	search_box = search_page.column();
	new_page = app->page("New chat");
	new_page.field("A name or a number", [](const std::string &t, bool) { new_text = t; contacts_filter(); });
	new_note = new_page.caption("Nobody yet: contacts arrive from your phone.");
	list_new = new_page.list(0, [](int) { return 72.0; }, [](int r, Node n) {
		if (size_t(r) >= contacts_shown.size()) return;
		const Contact c = contacts_shown[size_t(r)];
		Node it = n.item(c.name, c.phone, [jid = c.jid, name = c.name] { show_chat(jid, name); });
		avatar(it, c.jid, c.name);
	});
	status_page = app->page("Status");
	status_box = status_page.column();
	post_page = app->page("New status");
	static std::string post_text;
	Node post_field = post_page.field("What is on your mind", [](const std::string &t, bool) { post_text = t; });
	post_page.button("Post", [post_field]() mutable {
		if (post_text.empty()) return;
		if (post_text.size() > 700) { app->toast("A status holds 700 bytes"); return; }
		request("post_status", {{"text", post_text}}, [post_field](const json *, const char *err) mutable {
			if (err) { app->toast("Not posted. Your text is kept"); return; }
			post_field.set_text("");
			post_text.clear();
			load_statuses();
			app->back();
		});
	});
	post_note_n = post_page.caption("");
	info_page = app->page("Info");
	info_box = info_page.column();
	account_page = app->page("Account");
	account_box = account_page.column();
	image_page = app->page("Photo");
	image_box = image_page.column();
	(void)system_accent;
}

}	// namespace

int main(int argc, char **argv) {
	if (argc >= 2 && !strcmp(argv[1], "--daemon")) {
		argv[1] = argv[0];
		return ztwhatsappd_main(argc - 1, argv + 1);
	}
	if (argc >= 2 && !strcmp(argv[1], "--notifier")) {
		/* one at a time, whoever started it */
		std::string lock = run_file("zrn-whatsapp-notify.lock");
		int fd = open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) < 0) return 0;
		{
			const char *home = getenv("HOME");
			std::string ui = std::string("zrn-exec ") + (home ? home : "") + "/.local/share/zrn/apps/zrn-whatsapp/zrn-whatsapp.zexec";
			setenv("ZTWHATSAPP_UI", ui.c_str(), 0);
		}
		argv[1] = argv[0];
		return ztwhatsapp_notify_main(argc - 1, argv + 1);
	}
	signal(SIGPIPE, SIG_IGN);
	static zrn::ui::App application("zrn-whatsapp", "WhatsApp");
	app = &application;
	if (FILE *f = fopen(prefs_path().c_str(), "r")) {
		char line[64];
		while (fgets(line, sizeof(line), f)) if (!strncmp(line, "follow_system_accent=1", 22)) follow_accent = true;
		else if (!strncmp(line, "enter_sends=", 12)) pref_enter = line[12] == '1';
		else if (!strncmp(line, "load_stickers=", 14)) pref_stickers = line[14] == '1';
		else if (!strncmp(line, "notify=", 7)) pref_notify = line[7] == '1';
		else if (!strncmp(line, "notify_preview=", 15)) pref_preview = line[15] == '1';
		fclose(f);
	}
	accent_apply();
	build_pages(0);
	if (argc == 2 && argv[1][0] != '-') pending_open = argv[1];
	open_file_check();
	int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	std::string dir = run_file("");
	if (ino >= 0 && inotify_add_watch(ino, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) >= 0)
		zui_watch_fd(app->get(), ino, run_dir_event, nullptr);
	atexit(focus_clear);
	replace_old_roles();
	if (!getenv("ZTWHATSAPP_NO_SPAWN")) spawn_role("--notifier");	/* one at a time: its lock */
	if (!connect_daemon()) start_services();
	app->timer(2000, true, [] { tick(); });
	/* a quick first look for the service once it has been started */
	app->timer(300, false, [] { tick(); });
	app->timer(900, false, [] { tick(); });
	return app->run(main_page);
}
