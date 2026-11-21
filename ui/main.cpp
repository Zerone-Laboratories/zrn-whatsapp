// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * WhatsApp for ztOS, on zrnui v2 in the Metro style of Windows Phone 8.1 (docs/UI-SPEC.md). An unofficial client.
 *
 * All data comes from the engine over the socket in IPC.md. One binary, three roles:
 *	zrn-whatsapp [JID]		the app; starts the other two if they are not running
 *	zrn-whatsapp --daemon ...	the engine (ztwhatsappd)
 *	zrn-whatsapp --notifier ...	the notifier (ztwhatsapp-notify)
 *
 * Never log message text, names or numbers.
 */
#include "wa.h"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zrnapp.h>

int ztwhatsappd_main(int argc, char** argv);
int ztwhatsapp_notify_main(int argc, char** argv);

namespace wa {

State st;
Zid g_root;
MediaView g_media_view;
int g_status_person = -1;

namespace {

Zid g_frame;
bool g_link_root;
std::deque<std::string> g_params;
std::string g_pending_open;

std::string prefs_path() {
	const char* home = getenv("HOME");
	return std::string(home ? home : "") + "/.config/zrn-whatsapp/prefs";
}

bool g_was_linked;

void prefs_load() {
	FILE* f = fopen(prefs_path().c_str(), "r");
	if (!f) return;
	char line[96];
	bool b_follow = true;
	while (fgets(line, sizeof line, f)) {
		auto on = [&](const char* key) {
			const size_t n = strlen(key);
			return !strncmp(line, key, n) && line[n] == '1';
		};
		auto has = [&](const char* key) { return !strncmp(line, key, strlen(key)); };
		if (has("follow_system_accent=")) b_follow = on("follow_system_accent=");
		else if (has("whatsapp_green=")) st.prefs.b_green = on("whatsapp_green=");
		else if (has("enter_sends=")) st.prefs.b_enter_sends = on("enter_sends=");
		else if (has("load_stickers=")) st.prefs.b_stickers = on("load_stickers=");
		else if (has("notify=")) st.prefs.b_notify = on("notify=");
		else if (has("notify_preview=")) st.prefs.b_preview = on("notify_preview=");
		else if (has("linked=")) g_was_linked = on("linked=");
	}
	fclose(f);
	/* v0.2 kept green as the default with "follow_system_accent"; the accent is the default now (spec 3.2) */
	(void)b_follow;
}

void focus_clear() { unlink(run_file("ztwhatsapp.focus").c_str()); }

/* After an upgrade the engine and notifier of the old version are still running, and the engine's lock keeps the
 * new one out. Anything of ours from another binary goes. */
void replace_old_roles() {
	if (getenv("ZTWHATSAPP_NO_SPAWN")) return;
	char self[1024];
	const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n <= 0) return;
	self[n] = 0;
	std::vector<pid_t> old;
	DIR* d = opendir("/proc");
	while (dirent* e = d ? readdir(d) : nullptr) {
		const pid_t pid = atoi(e->d_name);
		if (pid <= 0 || pid == getpid()) continue;
		char path[64];
		char exe[1024];
		snprintf(path, sizeof(path), "/proc/%d/exe", int(pid));
		const ssize_t k = readlink(path, exe, sizeof(exe) - 1);
		if (k <= 0) continue;
		exe[k] = 0;
		const std::string name = exe;
		if (name.find("/bin/zrn-whatsapp") == std::string::npos || name == self) continue;
		/* the login units' engine and notifier are systemd's to run: a SIGTERM made them exit 0 and stay down
		 * (Restart=on-failure), so no messages came in the background. Kept unless asked (a staged test build). */
		if (!getenv("ZTWHATSAPP_REPLACE_SERVICE")) {
			char cg[64];
			snprintf(cg, sizeof(cg), "/proc/%d/cgroup", int(pid));
			std::string groups;
			if (FILE* f = fopen(cg, "r")) {
				char line[512];
				while (fgets(line, sizeof line, f)) groups += line;
				fclose(f);
			}
			if (groups.find("zrn-ztwhatsappd.service") != std::string::npos || groups.find("zrn-ztwhatsapp-notify.service") != std::string::npos)
				continue;
		}
		old.push_back(pid);
	}
	if (d) closedir(d);
	for (const pid_t pid : old) kill(pid, SIGTERM);
	for (int i = 0; i < 20 && !old.empty(); i++) {
		usleep(100000);
		old.erase(std::remove_if(old.begin(), old.end(), [](pid_t p) { return kill(p, 0) != 0; }), old.end());
	}
	for (const pid_t pid : old) kill(pid, SIGKILL);
}

void spawn_notifier() {
	if (getenv("ZTWHATSAPP_NO_SPAWN")) return;
	if (start_unit("zrn-ztwhatsapp-notify.service")) return;
	char self[1024];
	const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n <= 0) return;
	self[n] = 0;
	const pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			const int null = open("/dev/null", O_RDWR);
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
			for (int fd = 3; fd < 256; fd++) close(fd);
			execl(self, self, "--notifier", nullptr);
		}
		_exit(0);
	}
	if (pid > 0) waitpid(pid, nullptr, 0);
}

bool is_paired_state(const std::string& s) { return s == "paired" || s.empty(); }

void open_pending() {
	if (g_pending_open.empty() || !st.b_chats_loaded || g_link_root) return;
	const std::string id = g_pending_open;
	g_pending_open.clear();
	if (chat_page_shown() && open_chat_id() == id) return;
	show_main_root(id);
}

}  // namespace

/* ---- json ---- */
std::string jstr(const json& o, const char* const k) {
	auto it = o.find(k);
	return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}
long jint(const json& o, const char* const k) {
	auto it = o.find(k);
	return it != o.end() && it->is_number() ? it->get<long>() : 0;
}
bool jbool(const json& o, const char* const k) {
	auto it = o.find(k);
	return it != o.end() && it->is_boolean() && it->get<bool>();
}

Chat chat_from(const json& o) {
	Chat c;
	c.id = jstr(o, "id");
	c.name = jstr(o, "name");
	c.last_text = jstr(o, "last_text");
	c.b_group = jbool(o, "is_group");
	c.b_last_from_me = jbool(o, "last_from_me");
	c.b_muted = jbool(o, "muted");
	c.b_pinned = jbool(o, "pinned");
	c.b_archived = jbool(o, "archived");
	c.unread = int(jint(o, "unread"));
	c.last_ts = jint(o, "last_ts");
	if (c.name.empty()) c.name = c.id.substr(0, c.id.find('@'));
	return c;
}

Msg msg_from(const json& o) {
	Msg m;
	m.id = jstr(o, "id");
	m.chat = jstr(o, "chat");
	m.sender = jstr(o, "sender");
	m.sender_name = jstr(o, "sender_name");
	m.kind = jstr(o, "kind");
	m.text = jstr(o, "text");
	m.status = jstr(o, "status");
	m.quoted_id = jstr(o, "quoted_id");
	m.b_from_me = jbool(o, "from_me");
	m.b_starred = jbool(o, "starred");
	m.b_edited = jbool(o, "edited");
	m.ts = jint(o, "timestamp");
	if (m.kind == "system" && (m.text == "This message was deleted" || m.text == "You deleted this message")) {
		m.b_deleted = true;
		m.kind = "deleted";
	}
	auto media = o.find("media");
	if (media != o.end() && media->is_object()) {
		m.b_media = true;
		m.media.mime = jstr(*media, "mime");
		m.media.thumb = jstr(*media, "thumb_b64");
		m.media.path = jstr(*media, "path");
		m.media.file_name = jstr(*media, "file_name");
		m.media.width = int(jint(*media, "width"));
		m.media.height = int(jint(*media, "height"));
		m.media.size = jint(*media, "size");
		m.media.seconds = int(jint(*media, "seconds"));
		m.media.b_ptt = jbool(*media, "ptt");
		m.media.b_played = jbool(*media, "played");
		m.media.b_animated = jbool(*media, "animated");
		m.media.waveform = jstr(*media, "waveform");
	}
	return m;
}

std::vector<const Chat*> chats_sorted(const bool b_archived) {
	std::vector<const Chat*> v;
	for (auto& [id, c] : st.chats)
		if (c.b_archived == b_archived) v.push_back(&c);
	std::stable_sort(v.begin(), v.end(), [](const Chat* a, const Chat* b) {
		if (a->b_pinned != b->b_pinned) return a->b_pinned;
		return a->last_ts > b->last_ts;
	});
	return v;
}

std::string time_hm(const long ts) {
	const time_t t = ts;
	struct tm a;
	localtime_r(&t, &a);
	char out[16];
	strftime(out, sizeof out, "%H:%M", &a);
	return out;
}

/* chat list: today the time, this week the day, else the date (WP: lower case) */
std::string time_short(const long ts) {
	if (!ts) return "";
	const time_t t = ts, now = time(nullptr);
	struct tm a;
	struct tm b;
	localtime_r(&t, &a);
	localtime_r(&now, &b);
	char out[32];
	if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) strftime(out, sizeof out, "%H:%M", &a);
	else if (now - ts < 6 * 86400) strftime(out, sizeof out, "%a", &a);
	else strftime(out, sizeof out, "%d/%m/%y", &a);
	return lower(out);
}

std::string day_label(const long ts) {
	const time_t t = ts, now = time(nullptr);
	struct tm a;
	struct tm b;
	localtime_r(&t, &a);
	localtime_r(&now, &b);
	const int days = (b.tm_year - a.tm_year) * 366 + (b.tm_yday - a.tm_yday);
	char out[48];
	if (days == 0) return "today";
	if (days == 1) return "yesterday";
	if (days > 1 && days < 7) strftime(out, sizeof out, "%A", &a);
	else strftime(out, sizeof out, "%-d %B %Y", &a);
	return lower(out);
}

std::string one_line(std::string t) {
	std::replace(t.begin(), t.end(), '\n', ' ');
	if (t.size() > 200) {
		size_t c = 200;
		while (c && (static_cast<unsigned char>(t[c]) & 0xc0) == 0x80) c--;
		t.resize(c);
	}
	return t;
}

std::string first_letter(const std::string& n) {
	if (n.empty() || isdigit(static_cast<unsigned char>(n[0])) || n[0] == '+') return "#";
	size_t k = 1;
	while (k < n.size() && (static_cast<unsigned char>(n[k]) & 0xc0) == 0x80) k++;
	std::string l = n.substr(0, k);
	if (k == 1) l[0] = char(toupper(static_cast<unsigned char>(l[0])));
	return l;
}

std::string lower(std::string s) {
	for (char& c : s)
		if (static_cast<unsigned char>(c) < 0x80) c = char(tolower(static_cast<unsigned char>(c)));
	return s;
}
std::string upper(std::string s) {
	for (char& c : s)
		if (static_cast<unsigned char>(c) < 0x80) c = char(toupper(static_cast<unsigned char>(c)));
	return s;
}

/* 1-3 emoji and nothing else (spec 3.3): pictographs, with their joiners, selectors and skin tones */
bool emoji_only(const std::string& s, int* const count) {
	int n = 0;
	size_t i = 0;
	bool b_joined = false;
	while (i < s.size()) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		uint32_t cp;
		int len;
		if (c < 0x80) { cp = c; len = 1; }
		else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; len = 2; }
		else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; len = 3; }
		else { cp = c & 0x07; len = 4; }
		for (int k = 1; k < len && i + size_t(k) < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3f);
		i += size_t(len);
		if (cp == ' ') continue;
		if (cp == 0x200D) { b_joined = true; continue; }
		if (cp == 0xFE0F || (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F)) continue;
		const bool b_emoji = (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF) ||
			cp == 0x00A9 || cp == 0x00AE || (cp >= 0x2190 && cp <= 0x21FF) || (cp >= 0x1F1E6 && cp <= 0x1F1FF);
		if (!b_emoji) return false;
		if (!b_joined) n++;
		b_joined = false;
	}
	*count = n;
	return n >= 1 && n <= 3;
}

void prefs_save() {
	const char* home = getenv("HOME");
	mkdir((std::string(home ? home : "") + "/.config/zrn-whatsapp").c_str(), 0700);
	const std::string tmp = prefs_path() + ".new";
	if (FILE* f = fopen(tmp.c_str(), "w")) {
		/* the notifier reads notify and notify_preview from here */
		fprintf(f, "follow_system_accent=%d\nwhatsapp_green=%d\nenter_sends=%d\nload_stickers=%d\nnotify=%d\nnotify_preview=%d\nlinked=%d\n",
			!st.prefs.b_green, st.prefs.b_green, st.prefs.b_enter_sends, st.prefs.b_stickers, st.prefs.b_notify, st.prefs.b_preview,
			g_was_linked);
		fclose(f);
		rename(tmp.c_str(), prefs_path().c_str());
	}
}

bool g_main_pending = false;

/* the first main page, once (startup with a linked phone) */
void first_main() {
	if (!g_main_pending || !Zelement_is_valid(g_frame)) return;
	g_main_pending = false;
	Zframe_navigate(g_frame, page_main, &g_params.back());
}

/* The first chats of the last run: the list the app opens with, so its entrance turns real rows while the engine's
 * answer waits for the animations to end. Kept per engine socket (a test engine has its own). */
static std::string chats_cache_path() {
	const char* home = getenv("HOME");
	const std::string dir = std::string(home ? home : "") + "/.cache/zrn-whatsapp";
	mkdir((std::string(home ? home : "") + "/.cache").c_str(), 0700);
	mkdir(dir.c_str(), 0700);
	return dir + (getenv("ZTWHATSAPP_SOCK") ? "/chats-alt.json" : "/chats.json");
}
static constexpr size_t CHATS_CACHED = 24;

static void chats_cache_load() {
	FILE* f = fopen(chats_cache_path().c_str(), "r");
	if (!f) return;
	std::string text;
	char buf[16384];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
	fclose(f);
	const json a = json::parse(text, nullptr, false);
	if (!a.is_array()) return;
	for (auto& o : a) {
		Chat c = chat_from(o);
		if (!c.id.empty()) st.chats[c.id] = std::move(c);
	}
}

/* on the worker thread: the first chats as the engine sorted them (pinned, then newest) */
static void chats_cache_save(json chats) {
	work([chats = std::move(chats)] {
		const std::string path = chats_cache_path(), tmp = path + ".new";
		const int fd = open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
		if (fd < 0) return;
		const std::string text = chats.dump(-1, ' ', false, json::error_handler_t::replace);
		const bool b_ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
		close(fd);
		if (b_ok) rename(tmp.c_str(), path.c_str());
	});
}

void load_chats() {
	request("chats", {{"limit", 500}}, [](const json* r, const char* err) {
		if (err || !r->contains("chats")) return;
		st.chats.clear();
		std::map<std::string, const json*> raw;
		for (auto& o : (*r)["chats"]) {
			Chat c = chat_from(o);
			if (c.id.empty()) continue;
			raw[c.id] = &o;
			st.chats[c.id] = std::move(c);
		}
		{
			json first = json::array();
			for (const Chat* c : chats_sorted(false)) {
				if (first.size() >= CHATS_CACHED) break;
				first.push_back(*raw[c->id]);
			}
			chats_cache_save(std::move(first));
		}
		st.b_chats_loaded = true;
		main_chats_changed("");
		first_main();
		open_pending();
	});
}

void load_statuses() {
	request("statuses", json::object(), [](const json* r, const char* err) {
		if (err || !r->contains("contacts")) return;
		st.statuses.clear();
		for (auto& c : (*r)["contacts"]) {
			StatusPerson p;
			p.sender = jstr(c, "sender");
			p.name = jstr(c, "name");
			p.last_ts = jint(c, "last_ts");
			p.unseen = int(jint(c, "unseen"));
			if (c.contains("items"))
				for (auto& it : c["items"]) {
					StatusItem si;
					si.id = jstr(it, "id");
					si.text = jstr(it, "text");
					si.kind = jstr(it, "kind");
					si.ts = jint(it, "timestamp");
					si.b_seen = jbool(it, "seen");
					auto media = it.find("media");
					if (media != it.end() && media->is_object()) {
						si.thumb = jstr(*media, "thumb_b64");
						si.path = jstr(*media, "path");
					}
					p.items.push_back(std::move(si));
				}
			st.statuses.push_back(std::move(p));
		}
		st.b_statuses_loaded = true;
		main_statuses_changed();
	});
}

void load_channels() {
	request("channels", json::object(), [](const json* r, const char* err) {
		st.channels.clear();
		if (!err && r->contains("channels"))
			for (auto& o : (*r)["channels"]) st.channels.push_back({jstr(o, "id"), jstr(o, "name"), jstr(o, "description")});
		st.b_channels_loaded = true;
		main_channels_changed();
	});
}

void load_contacts() {
	request("contacts", {{"limit", 5000}}, [](const json* r, const char* err) {
		if (err || !r->contains("contacts")) return;
		st.contacts.clear();
		for (auto& c : (*r)["contacts"]) st.contacts.push_back({jstr(c, "jid"), jstr(c, "name"), jstr(c, "phone")});
		st.b_contacts_loaded = true;
		new_chat_contacts_changed();
	});
}

/* ---- navigation ---- */
Zid frame() { return g_frame; }

void go_back() {
	if (Zframe_can_go_back(g_frame)) Zframe_go_back(g_frame);
}

void nav(const ZPageFactory factory, const std::string& parameter, const ZPageTransition transition) {
	g_params.push_back(parameter);
	if (g_params.size() > 4096) g_params.pop_front(); /* (a deque keeps the rest where they are) */
	gap::navigate_with(g_frame, factory, &g_params.back(), transition);
}

const std::string& page_param(const Zid page) {
	static const std::string none;
	const auto* p = static_cast<const std::string*>(Zpage_get_parameter(page));
	return p ? *p : none;
}

/* A new frame drops the whole back stack (the SDK has no Frame.BackStack.Clear): the link page as the root while
 * unlinked, the main page as the root after the link or a notification. Done after the current handler returns. */
void show_link_root() {
	g_main_pending = false;
	if (g_link_root && Zelement_is_valid(g_frame)) {
		link_changed();
		return;
	}
	g_link_root = true;
	post([] {
		g_frame = Zframe_create();
		Zroot_set_visual(g_root, g_frame);
		g_params.push_back("");
		Zframe_navigate(g_frame, page_link, &g_params.back());
	});
}

void show_main_root(const std::string& then_chat) {
	g_main_pending = false;
	g_link_root = false;
	post([then_chat] {
		g_frame = Zframe_create();
		Zroot_set_visual(g_root, g_frame);
		g_params.push_back("");
		Zframe_set_transition(g_frame, then_chat.empty() ? Z_TRANSITION_TURNSTILE : Z_TRANSITION_NONE);
		Zframe_navigate(g_frame, page_main, &g_params.back());
		Zframe_set_transition(g_frame, Z_TRANSITION_TURNSTILE);
		if (!then_chat.empty()) open_chat(then_chat);
	});
}

/* ---- what the engine says ---- */
void on_event(const json& o) {
	const std::string ev = jstr(o, "event");
	if (ev == "connection") {
		const std::string was = st.connection;
		st.connection = jstr(o, "state");
		if (st.connection == "logged_out") {
			st.b_logged_out = true;
			st.pair_state = "unpaired";
			g_was_linked = false;
			prefs_save();
			show_link_root();
		} else if (st.connection == "connected" && was != "connected") {
			load_chats();
			load_statuses();
			load_channels();
		}
		banners_update();
		main_engine_changed();
	} else if (ev == "pairing") {
		std::string s = jstr(o, "state");
		st.pair_error = jstr(o, "error");
		if (s == "failed") s = "unpaired";
		if (s == "qr") st.pair_qr = jstr(o, "qr");
		if (s == "phone_code") st.pair_code = jstr(o, "code");
		st.pair_state = s;
		if (s == "paired") {
			st.self_jid = jstr(o, "self");
			st.b_logged_out = false;
			if (!g_was_linked) {
				g_was_linked = true;
				prefs_save();
			}
			if (g_link_root) show_main_root();
			load_chats();
		} else {
			show_link_root();
		}
		banners_update();
	} else if (ev == "chat") {
		if (!o.contains("chat")) return;
		Chat c = chat_from(o["chat"]);
		if (c.id.empty()) return;
		const std::string id = c.id;
		st.chats[id] = std::move(c);
		main_chats_changed(id);
	} else if (ev == "message") {
		chat_on_message(o);
	} else if (ev == "receipt") {
		chat_on_receipt(o);
	} else if (ev == "presence") {
		const std::string chat = jstr(o, "chat");
		st.typing[chat] = jbool(o, "typing");
		std::string line;
		if (jbool(o, "available")) line = "online";
		else if (const long ls = jint(o, "last_seen")) line = "last seen " + day_label(ls) + " at " + time_hm(ls);
		st.presence[chat] = line;
		chat_on_presence(chat);
		main_chats_changed(chat);
	} else if (ev == "history_sync" || ev == "app_state") {
		if (jstr(o, "state") == "done") {
			load_chats();
			load_statuses();
		}
	} else if (ev == "status") {
		load_statuses();
	} else if (ev == "contacts") {
		if (st.b_contacts_loaded) load_contacts();
	} else if (ev == "picture") {
		picture_changed(jstr(o, "jid"));
	}
}

void on_engine(const bool b_up) {
	if (b_up) {
		request("set_presence", {{"available", true}});
		request("status", json::object(), [](const json* r, const char* err) {
			if (err) return;
			st.self_jid = jstr(*r, "self");
		});
	} else {
		fail_waiting();
		st.connection = "connecting";
	}
	banners_update();
	main_engine_changed();
	link_changed();
}

/* the notifier asks for a chat: ztwhatsapp.open holds its id */
void on_run_dir_change() {
	const std::string path = run_file("ztwhatsapp.open");
	FILE* f = fopen(path.c_str(), "r");
	if (!f) return;
	char line[512] = "";
	const char* got = fgets(line, sizeof line, f);
	fclose(f);
	unlink(path.c_str());
	if (!got) return;
	line[strcspn(line, " \r\n")] = 0;
	if (!line[0]) return;
	g_pending_open = line;
	open_pending();
}

void on_shell_config_change() { theme_apply(); }

}  // namespace wa

using namespace wa;

#include <png.h>
#include <chrono>
#include <thread>

/* A run without a window (ZWA_SELFTEST_DIR): software frames of 360 x 740 written as PNG at fixed steps, against
 * whatever engine the socket has (ztwhatsappd --fake). For the build machine, which has no compositor. */
static void selftest_shot(const char* dir, const char* name) {
	ZFrame f = {};
	Zroot_render(g_root, &f);
	if (!f.pixels) return;
	png_image img = {};
	img.version = PNG_IMAGE_VERSION;
	img.width = unsigned(f.width);
	img.height = unsigned(f.height);
	img.format = PNG_FORMAT_RGBA;
	png_image_write_to_file(&img, (std::string(dir) + "/" + name + ".png").c_str(), 0, f.pixels, f.stride, nullptr);
}
static void selftest_run(const int ms) {
	for (int t = 0; t < ms; t += 16) {
		io_pump();
		Zroot_tick(g_root, 16);
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
}
static int selftest(const char* dir) {
	Zroot_set_size(g_root, 360, 740);
	io_start(nullptr);
	selftest_run(2500);
	selftest_shot(dir, "1-start");
	if (st.pair_state != "paired") {
		request("pair_qr");
		selftest_run(1500);
		selftest_shot(dir, "1b-qr");
		for (int i = 0; i < 40 && st.pair_state != "paired"; i++) selftest_run(500);
		selftest_run(3000);
		selftest_shot(dir, "1c-main");
	}
	if (getenv("ZWA_SELFTEST_LIGHT")) Zroot_set_theme(g_root, Z_THEME_LIGHT);
	if (!st.chats.empty()) {
		open_chat(chats_sorted(false).front()->id);
		selftest_run(2500);
		selftest_shot(dir, "2-chat");
		chat_selftest(dir, [dir](const char* name) { selftest_shot(dir, name); }, [](int ms) { selftest_run(ms); });
		nav(page_info, open_chat_id());
		selftest_run(1500);
		selftest_shot(dir, "3-info");
		go_back();
		selftest_run(800);
		go_back();
		selftest_run(800);
	}
	nav(page_settings);
	selftest_run(800);
	selftest_shot(dir, "4-settings");
	nav(page_new_chat);
	selftest_run(1500);
	selftest_shot(dir, "5-newchat");
	io_stop();
	Zroot_destroy(g_root);
	Zshutdown();
	return 0;
}

/* ZWA_PERF_SCRIPT=1 (with ZRNUI_FRAME_LOG=1): a fixed run of the moves the frame-time budget is about (UI-SPEC 7.25), each
 * marked on stderr so the frame log can be cut into parts: a fling of the chat list, a chat opened and left, settings
 * opened and left, then the app quits. The fling is a finger: pointer events at 16 ms steps. */
static void perf_mark(const char* const what) {
	fprintf(stderr, "zwa-perf %s depth=%d shown=%d\n", what, Zframe_get_back_stack_depth(frame()), int(Zroot_get_visual(g_root) == frame()));
}
static void perf_drag(const float x, const float y0, const float y1, const int steps, const int at) {
	after(at, [x, y0] { Zroot_inject_pointer(g_root, Z_POINTER_MOVE, x, y0); Zroot_inject_pointer(g_root, Z_POINTER_DOWN, x, y0); });
	for (int i = 1; i <= steps; i++) {
		const float y = y0 + (y1 - y0) * float(i) / float(steps);
		after(at + i * 16, [x, y] { Zroot_inject_pointer(g_root, Z_POINTER_MOVE, x, y); });
	}
	after(at + steps * 16 + 8, [x, y1] { Zroot_inject_pointer(g_root, Z_POINTER_UP, x, y1); });
}
static void perf_script() {
	/* the fake engine starts unpaired: it pairs on the first QR request */
	after(500, [] { if (st.pair_state != "paired") request("pair_qr"); });
	int at = 6000;
	after(at, [] { perf_mark("fling-list"); });
	perf_drag(200, 600, 300, 8, at);
	after(at += 2000, [] {
		perf_mark("navigate-chat");
		const auto chats = chats_sorted(false);
		if (!chats.empty()) open_chat(chats.front()->id);
	});
	after(at += 2500, [] { perf_mark("fling-chat-up"); });
	perf_drag(200, 250, 650, 8, at);
	after(at += 2500, [] { perf_mark("fling-chat-down"); });
	perf_drag(200, 650, 250, 8, at);
	after(at += 2500, [] { perf_mark("back-chat"); go_back(); });
	for (int round = 0; round < 2; round++) {
		after(at += 2000, [] { perf_mark("navigate-settings"); nav(page_settings); });
		after(at += 2000, [] { perf_mark("back-settings"); go_back(); });
	}
	after(at += 2000, [] {
		perf_mark("navigate-chat2");
		const auto chats = chats_sorted(false);
		if (!chats.empty()) open_chat(chats.front()->id);
	});
	after(at += 2500, [] { perf_mark("back-chat2"); go_back(); });
	after(at += 2000, [] { perf_mark("end"); Zwindow_quit(); });
}

static int selftest(const char* dir);
int main(int argc, char** argv) {
	if (argc >= 2 && !strcmp(argv[1], "--daemon")) {
		argv[1] = argv[0];
		return ztwhatsappd_main(argc - 1, argv + 1);
	}
	if (argc >= 2 && !strcmp(argv[1], "--notifier")) {
		/* one at a time, whoever started it */
		const std::string lock = run_file("zrn-whatsapp-notify.lock");
		const int fd = open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) < 0) return 3; /* another copy runs: 3 is not restarted */
		/* the app is this same binary: a v2 app is a plain executable, no .zexec */
		char self[1024];
		const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
		if (n > 0) {
			self[n] = 0;
			setenv("ZTWHATSAPP_UI", self, 0);
		}
		argv[1] = argv[0];
		return ztwhatsapp_notify_main(argc - 1, argv + 1);
	}
	signal(SIGPIPE, SIG_IGN);
	prefs_load();
	if (argc == 2 && argv[1][0] != '-') g_pending_open = argv[1];
	replace_old_roles();
	spawn_notifier();
	if (!Zinit()) return 1;
	g_root = Zroot_create();
	Zroot_on_back(g_root, [](Zid, void*) { return chat_back(); }, nullptr);
	theme_apply();
	gap::on_theme_changed(g_root, [](Zid, void*) {
		if (st.prefs.b_green) Zroot_set_accent_color(g_root, literal(0xFF25D366));
	}, nullptr);
	/* the first page: the main pivot when this phone was linked before, else the link page until the engine says */
	if (g_was_linked || getenv("ZTWHATSAPP_ASSUME_LINKED")) {
		g_frame = Zframe_create();
		Zroot_set_visual(g_root, g_frame);
		g_params.push_back("");
		/* the page goes in when the chats are there (or after 400 ms): its entrance turns the rows, not an empty list that
		 * the rows then pop into */
		g_main_pending = true;
		/* the chats of the last run are there: the page goes in with them now, the engine's answer comes after */
		chats_cache_load();
		if (!st.chats.empty()) first_main();
		else after(400, [] { first_main(); });
	} else {
		g_link_root = true;
		g_frame = Zframe_create();
		Zroot_set_visual(g_root, g_frame);
		g_params.push_back("");
		Zframe_navigate(g_frame, page_link, &g_params.back());
	}
	on_run_dir_change();
	if (const char* dir = getenv("ZWA_SELFTEST_DIR")) {
		first_main();
		return selftest(dir);
	}
	ZWindowOptions options = {};
	options.title = "WhatsApp";
	options.on_connect = [](void* display, void*) {
		io_start(display);
		if (const char* p = getenv("ZWA_PERF_SCRIPT"); p && *p == '1') perf_script();
	};
	/* no 4x MSAA on the window: zrnui anti-aliases its shapes and text itself, so it changed nothing seen (crops compared),
	 * but it cost the GPU enough that frames waited in eglSwapBuffers (2.0 -> 0.7 ms) and missed vsync (12-15 -> 3 over
	 * 30+ ms in a chat scroll); ZRNAPP_MSAA=1 brings it back */
	setenv("ZRNAPP_MSAA", "0", 0);
	const bool b_ok = Zwindow_run_ex(g_root, &options);
	if (engine_up()) request("set_presence", {{"available", false}});
	io_stop();
	focus_clear();
	Zroot_destroy(g_root);
	Zshutdown();
	return b_ok ? 0 : 1;
}
