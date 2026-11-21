// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The io thread: the engine's socket (IPC.md), timers, the watches on the runtime directory and on the shell's
 * config, and a worker for decoding. It blocks in poll; nothing runs on a timer while idle.
 *
 * Work for the UI thread goes to a queue; the UI thread is woken through its own Wayland connection: a
 * wl_display_sync from this thread makes the compositor send a `done` that the window loop of zrnapp dispatches
 * on the UI thread, where the queue is drained. zrnapp has no other way in (no fd watch, no idle callback).
 */
#include "wa.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <queue>
#include <set>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>

namespace wa {
/* The installed program with its login units: systemd starts the role, so the role it runs is the one it watches
 * and restarts (a copy started here would hold the lock and keep the unit down). */
bool start_unit(const char* const unit) {
	char self[1024];
	const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n <= 0) return false;
	self[n] = 0;
	const char* home = getenv("HOME");
	const char* cfg = getenv("XDG_CONFIG_HOME");
	const std::string dir = cfg && *cfg ? std::string(cfg) + "/systemd/user/" : std::string(home ? home : "") + "/.config/systemd/user/";
	if (strcmp(self, "/usr/local/bin/zrn-whatsapp") != 0 || access((dir + unit).c_str(), R_OK) != 0) return false;
	const pid_t pid = fork();
	if (pid == 0) {
		const int null = open("/dev/null", O_RDWR);
		dup2(null, 0);
		dup2(null, 1);
		dup2(null, 2);
		/* reset-failed first: a unit left failed (exit 3 earlier) starts again */
		if (fork() == 0) {
			execlp("systemctl", "systemctl", "--user", "reset-failed", unit, nullptr);
			_exit(127);
		}
		wait(nullptr);
		execlp("systemctl", "systemctl", "--user", "--no-block", "start", unit, nullptr);
		_exit(127);
	}
	if (pid < 0) return false;
	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

namespace {

using Clock = std::chrono::steady_clock;

struct Timer {
	Clock::time_point due;
	int id;
	bool operator<(const Timer& o) const { return due > o.due; }
};

std::mutex g_lock;
std::deque<std::function<void()>> g_ui;          /* for the UI thread */
/* Work that may wait for the animations to settle (engine replies and events, decoded pictures): run by ui_drain only
 * while nothing animates, in slices of a few ms, oldest first; while something animates it waits, unless it has waited
 * longer than IDLE_MAX_WAIT (a long fling or a ticking control must not starve it). */
struct Idle {
	std::function<void()> fn;
	Clock::time_point at;
};
std::deque<Idle> g_idle;
constexpr auto IDLE_MAX_WAIT = std::chrono::milliseconds(900);
constexpr double IDLE_SLICE_MS = 8.0;     /* per drain at rest: a frame that starts then is still in time */
bool g_drain_log;                         /* ZRNUI_FRAME_LOG: a zwa-drain line for each drain over 2 ms */
Clock::time_point g_enter_until;           /* the shell's launch zoom (ZTMETRO_ENTER_MS) counts as an animation */
std::string g_out;                               /* lines for the engine */
std::priority_queue<Timer> g_timers;
std::map<int, std::function<void()>> g_timer_fn; /* UI thread only */
std::set<int> g_cancelled;
int g_next_timer = 1;
std::thread g_io;
std::atomic<bool> g_quit;
int g_wake_fd = -1;
struct wl_display* g_display;
std::atomic<bool> g_wake_pending;
Clock::time_point g_wake_sent;

std::thread g_worker;
std::mutex g_work_lock;
std::condition_variable g_work_cv;
std::deque<std::function<void()>> g_work;

/* UI thread */
std::map<int, Reply> g_waiting;
int g_next_id = 1;
std::atomic<bool> g_up;

void ui_drain();

void sync_done(void* data, struct wl_callback* cb, uint32_t serial) {
	(void)data;
	(void)serial;
	wl_callback_destroy(cb);
	g_wake_pending = false;
	ui_drain();
}
const struct wl_callback_listener g_sync_listener = {sync_done};

/* any thread */
void wake_ui() {
	if (!g_display) return;
	if (g_wake_pending.exchange(true)) return;
	g_wake_sent = Clock::now();
	struct wl_callback* cb = wl_display_sync(g_display);
	if (cb) wl_callback_add_listener(cb, &g_sync_listener, nullptr);
	wl_display_flush(g_display);
}

void wake_io() {
	if (g_wake_fd < 0) return;
	const uint64_t one = 1;
	if (write(g_wake_fd, &one, sizeof one) < 0) {}
}

void push_idle(std::function<void()> fn) {
	{
		std::lock_guard<std::mutex> l(g_lock);
		g_idle.push_back({std::move(fn), Clock::now()});
	}
	wake_ui();
}

void push_ui(std::function<void()> fn) {
	{
		std::lock_guard<std::mutex> l(g_lock);
		g_ui.push_back(std::move(fn));
	}
	wake_ui();
}

double ms_since(const Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

void ui_drain() {
	const Clock::time_point t0 = Clock::now();
	for (int round = 0; round < 64; round++) {
		std::deque<std::function<void()>> batch;
		{
			std::lock_guard<std::mutex> l(g_lock);
			batch.swap(g_ui);
		}
		if (batch.empty()) break;
		for (auto& fn : batch) fn();
	}
	const double light = ms_since(t0);
	/* the work that waits for the animations: none while they run (only what waited too long, one item), else slices */
	const bool b_anim = animating();
	int ran = 0;
	size_t left = 0;
	for (;;) {
		Idle it;
		{
			std::lock_guard<std::mutex> l(g_lock);
			left = g_idle.size();
			if (!left) break;
			if (b_anim ? (ran > 0 || Clock::now() - g_idle.front().at < IDLE_MAX_WAIT) : (ran > 0 && ms_since(t0) >= IDLE_SLICE_MS)) break;
			it = std::move(g_idle.front());
			g_idle.pop_front();
			left--;
		}
		it.fn();
		ran++;
	}
	const double all = ms_since(t0);
	if (g_drain_log && all > 2.0)
		fprintf(stderr, "zwa-drain ms=%.1f light=%.1f idle_ran=%d idle_left=%zu anim=%d\n", all, light, ran, left, int(b_anim));
	/* more is left: at rest the next slice comes at once (after the frame this one let through); while animating the io
	 * thread looks again every 32 ms */
	if (left && !b_anim) wake_ui();
}

std::string sock_path() {
	if (const char* o = getenv("ZTWHATSAPP_SOCK")) return o;
	return run_file("ztwhatsapp.sock");
}

int connect_engine() {
	const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return -1;
	struct sockaddr_un a {};
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path().c_str());
	if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* The engine as a detached child: it stays when the window closes, so notifications keep coming. */
void spawn_engine() {
	if (getenv("ZTWHATSAPP_NO_SPAWN")) return;
	if (start_unit("zrn-ztwhatsappd.service")) return;
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
			execl(self, self, "--daemon", nullptr);
		}
		_exit(0);
	}
	if (pid > 0) waitpid(pid, nullptr, 0);
}

void deliver_line(const char* const begin, const char* const end) {
	json o = json::parse(begin, end, nullptr, false);
	if (o.is_discarded() || !o.is_object()) return;
	push_idle([o = std::move(o)]() mutable {
		/* (a message event has an "id" too: the message's, a string) */
		if (o.contains("event")) {
			on_event(o);
			return;
		}
		auto idv = o.find("id");
		if (idv == o.end() || !idv->is_number_integer()) return;
		auto it = g_waiting.find(idv->get<int>());
		if (it == g_waiting.end()) return;
		Reply r = std::move(it->second);
		g_waiting.erase(it);
		if (jbool(o, "ok")) {
			static const json empty = json::object();
			r(o.contains("result") ? &o["result"] : &empty, nullptr);
		} else {
			const std::string e = jstr(o, "error");
			r(nullptr, e.empty() ? "internal" : e.c_str());
		}
	});
}

void io_main() {
	int sock = -1;
	std::string in;
	bool b_spawned = false;
	Clock::time_point next_try = Clock::now();
	const int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	int wd_run = -1;
	int wd_cfg = -1;
	if (ino >= 0) {
		wd_run = inotify_add_watch(ino, run_file("").c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
		const char* home = getenv("HOME");
		const std::string cfg = std::string(home ? home : "") + "/.config/ztmetro";
		wd_cfg = inotify_add_watch(ino, cfg.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
	}
	while (!g_quit) {
		const Clock::time_point now = Clock::now();
		if (sock < 0 && now >= next_try) {
			sock = connect_engine();
			if (sock >= 0) {
				in.clear();
				g_up = true;
				push_ui([] { on_engine(true); });
			} else {
				if (!b_spawned) {
					b_spawned = true;
					spawn_engine();
				}
				next_try = now + std::chrono::milliseconds(700);
			}
		}
		int timeout = 1000;
		{
			std::lock_guard<std::mutex> l(g_lock);
			while (!g_timers.empty() && g_timers.top().due <= now) {
				const int id = g_timers.top().id;
				g_timers.pop();
				g_ui.push_back([id] {
					auto it = g_timer_fn.find(id);
					if (it == g_timer_fn.end()) return;
					auto fn = std::move(it->second);
					g_timer_fn.erase(it);
					if (g_cancelled.erase(id)) return;
					fn();
				});
			}
			if (!g_timers.empty()) {
				const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(g_timers.top().due - now).count();
				timeout = int(std::min<long long>(timeout, std::max<long long>(ms, 0)));
			}
			if (!g_ui.empty()) {
				/* a wake that got lost (its done came before the listener): send another */
				if (!g_wake_pending || Clock::now() - g_wake_sent > std::chrono::milliseconds(80)) g_wake_pending = false;
				timeout = std::min(timeout, 40);
			}
			if (!g_idle.empty()) {
				/* waiting for the animations to end: look again every 32 ms */
				if (Clock::now() - g_wake_sent >= std::chrono::milliseconds(32)) g_wake_pending = false;
				timeout = std::min(timeout, 32);
			}
			if (sock < 0) timeout = std::min(timeout, int(std::max<long long>(0,
				std::chrono::duration_cast<std::chrono::milliseconds>(next_try - now).count())));
		}
		{
			bool b_ui;
			{
				std::lock_guard<std::mutex> l(g_lock);
				b_ui = !g_ui.empty() || !g_idle.empty();
			}
			if (b_ui) wake_ui();
		}
		struct pollfd p[3];
		int n = 0;
		p[n++] = {g_wake_fd, POLLIN, 0};
		const int i_sock = sock >= 0 ? n : -1;
		if (sock >= 0) {
			std::lock_guard<std::mutex> l(g_lock);
			p[n++] = {sock, short(POLLIN | (g_out.empty() ? 0 : POLLOUT)), 0};
		}
		const int i_ino = ino >= 0 ? n : -1;
		if (ino >= 0) p[n++] = {ino, POLLIN, 0};
		if (poll(p, nfds_t(n), timeout) < 0 && errno != EINTR) break;
		if (p[0].revents & POLLIN) {
			uint64_t v;
			if (read(g_wake_fd, &v, sizeof v) < 0) {}
		}
		if (i_ino >= 0 && (p[i_ino].revents & POLLIN)) {
			alignas(struct inotify_event) char buf[4096];
			bool b_run = false;
			bool b_cfg = false;
			ssize_t got;
			while ((got = read(ino, buf, sizeof buf)) > 0) {
				for (char* q = buf; q < buf + got;) {
					auto* e = reinterpret_cast<struct inotify_event*>(q);
					if (e->wd == wd_run && e->len && !strcmp(e->name, "ztwhatsapp.open")) b_run = true;
					if (e->wd == wd_cfg && e->len && !strcmp(e->name, "config")) b_cfg = true;
					q += sizeof(struct inotify_event) + e->len;
				}
			}
			if (b_run) push_ui([] { on_run_dir_change(); });
			if (b_cfg) push_ui([] { on_shell_config_change(); });
		}
		if (sock < 0) continue;
		bool b_gone = false;
		if (i_sock >= 0 && (p[i_sock].revents & (POLLIN | POLLHUP | POLLERR))) {
			char chunk[65536];
			for (;;) {
				const ssize_t r = recv(sock, chunk, sizeof chunk, MSG_DONTWAIT);
				if (r > 0) {
					in.append(chunk, size_t(r));
					continue;
				}
				if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) b_gone = true;
				break;
			}
			size_t start = 0;
			size_t nl;
			while ((nl = in.find('\n', start)) != std::string::npos) {
				deliver_line(in.data() + start, in.data() + nl);
				start = nl + 1;
			}
			in.erase(0, start);
			if (in.size() > (8u << 20)) b_gone = true;
		}
		if (!b_gone) {
			std::lock_guard<std::mutex> l(g_lock);
			while (!g_out.empty()) {
				const ssize_t w = send(sock, g_out.data(), g_out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
				if (w <= 0) {
					if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) b_gone = true;
					break;
				}
				g_out.erase(0, size_t(w));
			}
		}
		if (b_gone) {
			close(sock);
			sock = -1;
			g_up = false;
			{
				std::lock_guard<std::mutex> l(g_lock);
				g_out.clear();
			}
			next_try = Clock::now() + std::chrono::milliseconds(500);
			push_ui([] { on_engine(false); });
		}
	}
	if (sock >= 0) close(sock);
	if (ino >= 0) close(ino);
}

void worker_main() {
	for (;;) {
		std::function<void()> fn;
		{
			std::unique_lock<std::mutex> l(g_work_lock);
			g_work_cv.wait(l, [] { return g_quit || !g_work.empty(); });
			if (g_quit) return;
			fn = std::move(g_work.front());
			g_work.pop_front();
		}
		fn();
	}
}

}  // namespace

std::string run_file(const char* const name) {
	const char* run = getenv("XDG_RUNTIME_DIR");
	return std::string(run ? run : "/tmp") + "/" + name;
}

void io_start(void* const wl_display) {
	g_drain_log = getenv("ZRNUI_FRAME_LOG") != nullptr;
	if (const char* enter = getenv("ZTMETRO_ENTER_MS")) {
		/* the shell's clock stamp (CLOCK_MONOTONIC ms, 32 bits) of the end of its launch zoom */
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		const uint32_t now = uint32_t(uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec) / 1000000u);
		const int32_t ahead = int32_t(uint32_t(strtoul(enter, nullptr, 10)) - now);
		if (ahead > 0 && ahead < 3000) g_enter_until = Clock::now() + std::chrono::milliseconds(ahead);
	}
	g_display = static_cast<struct wl_display*>(wl_display);
	g_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	g_io = std::thread(io_main);
	g_worker = std::thread(worker_main);
}

void io_stop() {
	if (!g_io.joinable()) return;
	g_quit = true;
	wake_io();
	g_work_cv.notify_all();
	g_io.join();
	g_worker.join();
}

bool engine_up() { return g_up; }

void io_pump() { ui_drain(); }

void request(const char* const cmd, json fields, Reply reply) {
	if (!g_up) {
		if (reply) reply(nullptr, "not_connected");
		return;
	}
	const int id = g_next_id++;
	fields["id"] = id;
	fields["cmd"] = cmd;
	if (reply) g_waiting[id] = std::move(reply);
	{
		std::lock_guard<std::mutex> l(g_lock);
		g_out += fields.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
	}
	wake_io();
}

/* the engine went away: every reply that was waiting fails (UI thread) */
void fail_waiting() {
	auto pending = std::move(g_waiting);
	g_waiting.clear();
	for (auto& [id, r] : pending) r(nullptr, "not_connected");
}

void post(std::function<void()> fn) { push_ui(std::move(fn)); }
void post_idle(std::function<void()> fn) { push_idle(std::move(fn)); }

bool animating() {
	if (Clock::now() < g_enter_until) return true;
	return g_root != Z_INVALID_ID && Zroot_is_animating(g_root);
}

int after(const int ms, std::function<void()> fn) {
	std::lock_guard<std::mutex> l(g_lock);
	const int id = g_next_timer++;
	g_timer_fn[id] = std::move(fn);
	g_timers.push({Clock::now() + std::chrono::milliseconds(ms), id});
	wake_io();
	return id;
}

void cancel(const int timer) {
	if (timer <= 0) return;
	std::lock_guard<std::mutex> l(g_lock);
	if (g_timer_fn.count(timer)) g_cancelled.insert(timer);
}

void work(std::function<void()> fn) {
	{
		std::lock_guard<std::mutex> l(g_work_lock);
		g_work.push_back(std::move(fn));
	}
	g_work_cv.notify_one();
}

}  // namespace wa
