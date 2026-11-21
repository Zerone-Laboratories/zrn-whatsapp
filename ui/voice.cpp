// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Voice notes: recording and playback through the system's default devices (PipeWire, or PulseAudio), with the
 * system's own tools, so the app links no audio library:
 *   record  pw-record (else parecord) to a WAV in the runtime directory, then ffmpeg makes the Opus-in-OGG that
 *           WhatsApp sends as a PTT; the waveform (64 values 0-100) and the length come from the WAV.
 *   play    ffmpeg decodes a note once to raw 48 kHz mono (cached in the runtime directory); pw-play (else pacat)
 *           reads it from a pipe a worker feeds from any offset, so pause and seek are a stop and a start.
 * Nothing here runs on the UI thread except starting and stopping; the results come back through wa::post.
 */
#include "wa.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace wa::voice {
namespace {

constexpr int RATE = 48000;
constexpr double BYTES_PER_S = RATE * 2.0;

bool have(const char* tool) {
	const char* p = getenv("PATH");
	std::string all = p ? p : "/usr/bin:/bin";
	size_t at = 0;
	while (at <= all.size()) {
		const size_t end = all.find(':', at);
		const std::string dir = all.substr(at, end == std::string::npos ? std::string::npos : end - at);
		if (!dir.empty() && access((dir + "/" + tool).c_str(), X_OK) == 0) return true;
		if (end == std::string::npos) break;
		at = end + 1;
	}
	return false;
}

/* a child with stdin from `in_fd` (or /dev/null) and stdout/stderr to /dev/null */
pid_t spawn(const std::vector<std::string>& argv, const int in_fd = -1) {
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	if (in_fd >= 0) posix_spawn_file_actions_adddup2(&fa, in_fd, 0);
	else posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	std::vector<char*> v;
	for (const std::string& a : argv) v.push_back(const_cast<char*>(a.c_str()));
	v.push_back(nullptr);
	pid_t pid = -1;
	if (posix_spawnp(&pid, v[0], &fa, nullptr, v.data(), environ) != 0) pid = -1;
	posix_spawn_file_actions_destroy(&fa);
	return pid;
}

int run_wait(const std::vector<std::string>& argv) {
	const pid_t pid = spawn(argv);
	if (pid < 0) return -1;
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string dir() {
	const std::string d = run_file("zrn-whatsapp-voice");
	mkdir(d.c_str(), 0700);
	return d;
}

/* the PCM of a WAV: 16-bit, the channels and the rate from its header; a header left unfinished by a stop is tolerated */
bool wav_read(const std::string& path, std::vector<int16_t>* pcm, int* rate) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	std::vector<uint8_t> raw;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) raw.insert(raw.end(), buf, buf + n);
	fclose(f);
	if (raw.size() < 44 || memcmp(raw.data(), "RIFF", 4) || memcmp(raw.data() + 8, "WAVE", 4)) return false;
	int channels = 1, bits = 16;
	*rate = RATE;
	size_t at = 12;
	while (at + 8 <= raw.size()) {
		const uint32_t len = raw[at + 4] | raw[at + 5] << 8 | raw[at + 6] << 16 | uint32_t(raw[at + 7]) << 24;
		if (!memcmp(raw.data() + at, "fmt ", 4) && at + 24 <= raw.size()) {
			channels = raw[at + 10] | raw[at + 11] << 8;
			*rate = int(raw[at + 12] | raw[at + 13] << 8 | raw[at + 14] << 16 | uint32_t(raw[at + 15]) << 24);
			bits = raw[at + 22] | raw[at + 23] << 8;
		} else if (!memcmp(raw.data() + at, "data", 4)) {
			if (bits != 16 || channels < 1 || *rate <= 0) return false;
			size_t end = len && len != 0xFFFFFFFFu ? std::min(raw.size(), at + 8 + size_t(len)) : raw.size();
			for (size_t i = at + 8; i + size_t(channels) * 2 <= end; i += size_t(channels) * 2)
				pcm->push_back(int16_t(raw[i] | raw[i + 1] << 8));
			return true;
		}
		at += 8 + len + (len & 1);
	}
	return false;
}

/* WhatsApp's waveform: 64 values 0-100, the loudness of each 64th of the note, the loudest at 100 */
std::string waveform_of(const std::vector<int16_t>& pcm) {
	std::vector<uint8_t> out(64, 0);
	if (pcm.empty()) return "";
	std::vector<double> rms(64, 0);
	double top = 1;
	for (size_t b = 0; b < 64; b++) {
		const size_t a = pcm.size() * b / 64, z = std::max(a + 1, pcm.size() * (b + 1) / 64);
		double acc = 0;
		for (size_t i = a; i < z && i < pcm.size(); i++) acc += double(pcm[i]) * double(pcm[i]);
		rms[b] = std::sqrt(acc / double(z - a));
		top = std::max(top, rms[b]);
	}
	for (size_t b = 0; b < 64; b++) out[b] = uint8_t(std::lround(100.0 * rms[b] / top));
	static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string s;
	for (size_t i = 0; i < out.size(); i += 3) {
		const unsigned v = unsigned(out[i]) << 16 | (i + 1 < out.size() ? unsigned(out[i + 1]) << 8 : 0) | (i + 2 < out.size() ? out[i + 2] : 0);
		s += T[(v >> 18) & 63];
		s += T[(v >> 12) & 63];
		s += i + 1 < out.size() ? T[(v >> 6) & 63] : '=';
		s += i + 2 < out.size() ? T[v & 63] : '=';
	}
	return s;
}

/* ---- recording ---- */
pid_t g_rec = -1;
std::string g_rec_wav;
long g_rec_start_ms;

long now_ms() {
	timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

void rec_end(const pid_t pid, const bool b_wait) {
	if (pid <= 0) return;
	kill(pid, SIGINT);
	if (b_wait) {
		/* pw-record writes the WAV's header when it ends: a short wait for it, then it goes */
		for (int i = 0; i < 100; i++) {
			if (waitpid(pid, nullptr, WNOHANG) == pid) return;
			usleep(10000);
		}
		kill(pid, SIGKILL);
		waitpid(pid, nullptr, 0);
	} else {
		std::thread([pid] {
			usleep(300000);
			kill(pid, SIGKILL);
			waitpid(pid, nullptr, 0);
		}).detach();
	}
}

/* ---- playback ---- */
struct Player {
	std::string key;
	pid_t pid = -1;
	int fd = -1;
	std::atomic<bool> b_stop{false};
	long start_ms = 0;
	double from = 0, dur = 0;
	int timer = 0;
	std::function<void(double, double, bool)> tick;
};
std::shared_ptr<Player> g_play;

void play_stop(const bool b_report) {
	auto p = g_play;
	if (!p) return;
	g_play.reset();
	p->b_stop = true;
	cancel(p->timer);
	if (p->pid > 0) kill(p->pid, SIGTERM);
	if (b_report && p->tick) {
		const double at = std::min(p->dur, p->from + double(now_ms() - p->start_ms) / 1000.0);
		p->tick(at, p->dur, false);
	}
}

void play_tick(const std::shared_ptr<Player>& p) {
	if (g_play != p) return;
	const double at = std::min(p->dur, p->from + double(now_ms() - p->start_ms) / 1000.0);
	if (p->tick) p->tick(at, p->dur, true);
	p->timer = after(100, [p] { play_tick(p); });
}

}  // namespace

bool can_record() { return (have("pw-record") || have("parecord")) && have("ffmpeg"); }

bool record_start() {
	if (g_rec > 0) return true;
	play_stop(true);
	g_rec_wav = dir() + "/rec-" + std::to_string(getpid()) + ".wav";
	unlink(g_rec_wav.c_str());
	if (have("pw-record"))
		g_rec = spawn({"pw-record", "--rate", std::to_string(RATE), "--channels", "1", "--format", "s16", "--media-role", "Communication", g_rec_wav});
	else
		g_rec = spawn({"parecord", "--rate=48000", "--channels=1", "--format=s16le", "--file-format=wav", g_rec_wav});
	g_rec_start_ms = now_ms();
	return g_rec > 0;
}

int record_ms() { return g_rec > 0 ? int(now_ms() - g_rec_start_ms) : 0; }

void record_cancel() {
	rec_end(g_rec, false);
	g_rec = -1;
	const std::string wav = g_rec_wav;
	after(500, [wav] { unlink(wav.c_str()); });
}

void record_stop(std::function<void(const Note&)> done) {
	const std::string wav = g_rec_wav;
	const int ms = record_ms();
	const pid_t pid = g_rec;
	g_rec = -1;
	work([wav, ms, pid, done = std::move(done)] {
		Note n;
		rec_end(pid, true);
		std::vector<int16_t> pcm;
		int rate = RATE;
		if (wav_read(wav, &pcm, &rate) && !pcm.empty()) {
			n.seconds = std::max(1, int(std::lround(double(pcm.size()) / rate)));
			n.waveform = waveform_of(pcm);
			const std::string ogg = wav.substr(0, wav.size() - 4) + "-" + std::to_string(time(nullptr)) + ".ogg";
			if (run_wait({"ffmpeg", "-y", "-loglevel", "error", "-i", wav, "-ac", "1", "-ar", "48000", "-c:a", "libopus", "-b:a", "24k",
					"-application", "voip", "-f", "ogg", ogg}) == 0)
				n.path = ogg;
		} else {
			n.seconds = std::max(1, ms / 1000);
		}
		unlink(wav.c_str());
		post([n, done] { done(n); });
	});
}

/* decodes once to the cache; `done` gets the raw path and the length in seconds (0 when it cannot be read) */
void prepare(const std::string& path, std::function<void(std::string, double)> done) {
	work([path, done = std::move(done)] {
		char name[32];
		snprintf(name, sizeof name, "%016zx.raw", std::hash<std::string>()(path));
		const std::string raw = dir() + "/" + name;
		struct stat sb;
		if (stat(raw.c_str(), &sb) != 0 || sb.st_size == 0) {
			const std::string tmp = raw + ".part";
			if (run_wait({"ffmpeg", "-y", "-loglevel", "error", "-i", path, "-ac", "1", "-ar", "48000", "-f", "s16le", tmp}) == 0) rename(tmp.c_str(), raw.c_str());
			else unlink(tmp.c_str());
		}
		const double dur = stat(raw.c_str(), &sb) == 0 ? double(sb.st_size) / BYTES_PER_S : 0;
		post([raw, dur, done] { done(dur > 0 ? raw : "", dur); });
	});
}

void play(const std::string& key, const std::string& path, double from, std::function<void(double pos, double dur, bool b_playing)> tick) {
	play_stop(true);
	auto p = std::make_shared<Player>();
	p->key = key;
	p->tick = std::move(tick);
	g_play = p;
	prepare(path, [p, from](const std::string& raw, const double dur) {
		if (g_play != p) return;
		if (raw.empty()) {
			g_play.reset();
			if (p->tick) p->tick(0, 0, false);
			return;
		}
		p->dur = dur;
		p->from = from >= dur - 0.05 ? 0 : std::max(0.0, from);
		int pipefd[2];
		if (pipe2(pipefd, O_CLOEXEC) != 0) return;
		p->pid = have("pw-play") ? spawn({"pw-play", "--raw", "--rate", "48000", "--channels", "1", "--format", "s16", "--media-role", "Music", "-"}, pipefd[0])
								 : spawn({"pacat", "--playback", "--raw", "--rate=48000", "--channels=1", "--format=s16le"}, pipefd[0]);
		close(pipefd[0]);
		if (p->pid < 0) {
			close(pipefd[1]);
			g_play.reset();
			if (p->tick) p->tick(p->from, dur, false);
			return;
		}
		p->fd = pipefd[1];
		p->start_ms = now_ms();
		const off_t at = off_t(p->from * BYTES_PER_S) & ~off_t(1);
		std::thread([p, raw, at] {
			const int in = open(raw.c_str(), O_RDONLY | O_CLOEXEC);
			if (in >= 0) {
				lseek(in, at, SEEK_SET);
				char buf[9600];
				ssize_t n;
				while (!p->b_stop && (n = read(in, buf, sizeof buf)) > 0)
					for (ssize_t w = 0; w < n && !p->b_stop;) {
						const ssize_t k = write(p->fd, buf + w, size_t(n - w));
						if (k <= 0) {
							p->b_stop = true;
							break;
						}
						w += k;
					}
				close(in);
			}
			close(p->fd);
			int status;
			waitpid(p->pid, &status, 0);
			post([p] {
				if (g_play != p) return;
				g_play.reset();
				cancel(p->timer);
				if (p->tick) p->tick(p->dur, p->dur, false);
			});
		}).detach();
		play_tick(p);
	});
}

void pause() { play_stop(true); }
void stop_all() {
	play_stop(false);
	if (g_rec > 0) record_cancel();
}
bool playing(const std::string& key) { return g_play && g_play->key == key; }

std::vector<int> waveform_decode(const std::string& b64) {
	std::vector<int> v;
	for (const uint8_t b : unbase64(b64)) v.push_back(std::min(100, int(b)));
	return v;
}

}  // namespace wa::voice
