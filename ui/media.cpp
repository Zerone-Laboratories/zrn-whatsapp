// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pictures: decoded on the worker thread, never on the UI thread, at the size they are shown at (times the UI
 * scale), kept in an LRU bounded to 64 MB (spec 7). Profile pictures are fetched one at a time.
 */
#include "wa.h"

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <deque>
#include <ctime>
#include <list>
#include <mutex>
#include <set>

#include <jpeglib.h>
#include <png.h>
#include <sys/stat.h>
#include <webp/decode.h>
#include <webp/demux.h>

#include "qr.h"

namespace wa {
namespace {

constexpr size_t CACHE_BYTES = 64u << 20;

std::mutex g_cache_lock;
std::list<std::pair<std::string, PixP>> g_lru;
std::map<std::string, std::list<std::pair<std::string, PixP>>::iterator> g_index;
size_t g_cache_size;

PixP cache_get(const std::string& key) {
	std::lock_guard<std::mutex> l(g_cache_lock);
	auto it = g_index.find(key);
	if (it == g_index.end()) return nullptr;
	g_lru.splice(g_lru.begin(), g_lru, it->second);
	return it->second->second;
}

void cache_put(const std::string& key, const PixP& p) {
	if (!p) return;
	std::lock_guard<std::mutex> l(g_cache_lock);
	if (g_index.count(key)) return;
	g_lru.emplace_front(key, p);
	g_index[key] = g_lru.begin();
	g_cache_size += p->px.size();
	while (g_cache_size > CACHE_BYTES && g_lru.size() > 1) {
		g_cache_size -= g_lru.back().second->px.size();
		g_index.erase(g_lru.back().first);
		g_lru.pop_back();
	}
}

std::vector<uint8_t> read_all(const std::string& path) {
	std::vector<uint8_t> raw;
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return raw;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0 && raw.size() < (32u << 20)) raw.insert(raw.end(), buf, buf + n);
	fclose(f);
	return raw;
}

struct JpegErr {
	jpeg_error_mgr mgr;
	jmp_buf jump;
};

/* JPEG with the decoder's own downscale (1/2, 1/4, 1/8) toward the wanted size first */
std::shared_ptr<Pix> jpeg(const std::vector<uint8_t>& raw, const int want_w, const int want_h) {
	jpeg_decompress_struct c;
	JpegErr err;
	c.err = jpeg_std_error(&err.mgr);
	err.mgr.error_exit = [](j_common_ptr ci) { longjmp(reinterpret_cast<JpegErr*>(ci->err)->jump, 1); };
	auto out = std::make_shared<Pix>();
	if (setjmp(err.jump)) {
		jpeg_destroy_decompress(&c);
		return nullptr;
	}
	jpeg_create_decompress(&c);
	jpeg_mem_src(&c, raw.data(), static_cast<unsigned long>(raw.size()));
	jpeg_read_header(&c, TRUE);
	c.out_color_space = JCS_EXT_RGBA;
	int denom = 1;
	while (denom < 8 && want_w > 0 && want_h > 0 && int(c.image_width) / (denom * 2) >= want_w && int(c.image_height) / (denom * 2) >= want_h)
		denom *= 2;
	c.scale_num = 1;
	c.scale_denom = unsigned(denom);
	jpeg_start_decompress(&c);
	out->w = int(c.output_width);
	out->h = int(c.output_height);
	out->px.resize(size_t(out->w) * size_t(out->h) * 4);
	while (c.output_scanline < c.output_height) {
		JSAMPROW row = &out->px[size_t(c.output_scanline) * size_t(out->w) * 4];
		jpeg_read_scanlines(&c, &row, 1);
	}
	jpeg_finish_decompress(&c);
	jpeg_destroy_decompress(&c);
	return out;
}

std::shared_ptr<Pix> png(const std::vector<uint8_t>& raw) {
	png_image img;
	memset(&img, 0, sizeof img);
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&img, raw.data(), raw.size())) return nullptr;
	img.format = PNG_FORMAT_RGBA;
	auto out = std::make_shared<Pix>();
	out->w = int(img.width);
	out->h = int(img.height);
	out->px.resize(PNG_IMAGE_SIZE(img));
	if (!png_image_finish_read(&img, nullptr, out->px.data(), 0, nullptr)) {
		png_image_free(&img);
		return nullptr;
	}
	return out;
}

/* WebP, the first frame of an animation (stickers): read once, nothing runs on a timer */
std::shared_ptr<Pix> webp(const std::vector<uint8_t>& raw) {
	WebPAnimDecoderOptions o;
	WebPAnimDecoderOptionsInit(&o);
	o.color_mode = MODE_RGBA;
	o.use_threads = 0;
	const WebPData d = {raw.data(), raw.size()};
	WebPAnimDecoder* dec = WebPAnimDecoderNew(&d, &o);
	if (!dec) return nullptr;
	WebPAnimInfo info;
	uint8_t* px = nullptr;
	int ts = 0;
	std::shared_ptr<Pix> out;
	if (WebPAnimDecoderGetInfo(dec, &info) && WebPAnimDecoderGetNext(dec, &px, &ts) && px) {
		out = std::make_shared<Pix>();
		out->w = int(info.canvas_width);
		out->h = int(info.canvas_height);
		out->px.assign(px, px + size_t(out->w) * size_t(out->h) * 4);
	}
	WebPAnimDecoderDelete(dec);
	return out;
}

/* area average down, bilinear up: the GL painter samples with NEAREST, so the bitmap must already be the size shown */
std::shared_ptr<Pix> resize(const std::shared_ptr<Pix>& s, const int w, const int h) {
	if (!s || w <= 0 || h <= 0 || (w == s->w && h == s->h)) return s;
	auto d = std::make_shared<Pix>();
	d->w = w;
	d->h = h;
	d->px.resize(size_t(w) * size_t(h) * 4);
	const float sx = float(s->w) / float(w);
	const float sy = float(s->h) / float(h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float acc[4] = {0, 0, 0, 0};
			if (sx > 1.0f || sy > 1.0f) {
				const int x0 = int(float(x) * sx), x1 = std::max(x0 + 1, std::min(s->w, int(float(x + 1) * sx)));
				const int y0 = int(float(y) * sy), y1 = std::max(y0 + 1, std::min(s->h, int(float(y + 1) * sy)));
				int n = 0;
				for (int yy = y0; yy < y1; yy++)
					for (int xx = x0; xx < x1; xx++, n++)
						for (int k = 0; k < 4; k++) acc[k] += s->px[(size_t(yy) * size_t(s->w) + size_t(xx)) * 4 + size_t(k)];
				for (float& a : acc) a /= float(n);
			} else {
				const float fx = std::max(0.0f, (float(x) + 0.5f) * sx - 0.5f), fy = std::max(0.0f, (float(y) + 0.5f) * sy - 0.5f);
				const int x0 = std::min(int(fx), s->w - 1), y0 = std::min(int(fy), s->h - 1);
				const int x1 = std::min(x0 + 1, s->w - 1), y1 = std::min(y0 + 1, s->h - 1);
				const float ax = fx - float(x0), ay = fy - float(y0);
				for (int k = 0; k < 4; k++) {
					auto P = [&](int xx, int yy) { return float(s->px[(size_t(yy) * size_t(s->w) + size_t(xx)) * 4 + size_t(k)]); };
					acc[k] = (P(x0, y0) * (1 - ax) + P(x1, y0) * ax) * (1 - ay) + (P(x0, y1) * (1 - ax) + P(x1, y1) * ax) * ay;
				}
			}
			for (int k = 0; k < 4; k++) d->px[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(k)] = uint8_t(std::clamp(acc[k] + 0.5f, 0.0f, 255.0f));
		}
	return d;
}

PixP decode(const std::vector<uint8_t>& raw, const float max_w, const float max_h, const bool b_cover) {
	if (raw.size() < 12) return nullptr;
	const float scale = ui_scale();
	const int want_w = int(std::ceil(max_w * scale)), want_h = int(std::ceil(max_h * scale));
	std::shared_ptr<Pix> p;
	if (raw[0] == 0xFF && raw[1] == 0xD8) p = jpeg(raw, want_w, want_h);
	else if (raw[0] == 0x89 && raw[1] == 'P') p = png(raw);
	else if (!memcmp(raw.data(), "RIFF", 4) && !memcmp(raw.data() + 8, "WEBP", 4)) p = webp(raw);
	if (!p || p->w <= 0 || p->h <= 0) return nullptr;
	if (want_w <= 0 || want_h <= 0) return p;
	float f = b_cover ? std::max(float(want_w) / float(p->w), float(want_h) / float(p->h))
					  : std::min(float(want_w) / float(p->w), float(want_h) / float(p->h));
	const int w = std::max(1, int(std::lround(float(p->w) * f)));
	const int h = std::max(1, int(std::lround(float(p->h) * f)));
	if (b_cover && (w > want_w || h > want_h)) {
		/* only the middle of a cover is seen (UniformToFill centres it): the rest is cut before scaling, so a tall photo in
		 * a 236 x 304 box is 944 x 1216 (4.5 MB) instead of 944 x 2098 (7.7 MB) to upload while the chat scrolls */
		const int cw = std::min(p->w, std::max(1, int(std::lround(float(want_w) / f))));
		const int ch = std::min(p->h, std::max(1, int(std::lround(float(want_h) / f))));
		const int cx = (p->w - cw) / 2, cy = (p->h - ch) / 2;
		auto c = std::make_shared<Pix>();
		c->w = cw;
		c->h = ch;
		c->px.resize(size_t(cw) * size_t(ch) * 4);
		for (int y = 0; y < ch; y++)
			memcpy(&c->px[size_t(y) * size_t(cw) * 4], &p->px[(size_t(cy + y) * size_t(p->w) + size_t(cx)) * 4], size_t(cw) * 4);
		return resize(c, std::min(w, want_w), std::min(h, want_h));
	}
	return resize(p, w, h);
}

/* ---- profile pictures, one fetch at a time ----
 * A picture once shown stays the jid's until a new one is fetched: a change (the picture event) or a failed fetch
 * never takes it away. Only "not_found" (none, or hidden by privacy) leaves the letter; anything else (the engine not
 * connected yet, a timeout, the server busy) is tried again a little later (QA: the chat header sometimes showed the
 * letter while the list showed the photo, after one fetch failed and the jid was marked as having none). */
std::map<std::string, std::vector<Zid>> g_pic_wanted; /* waiting for a picture */
std::map<std::string, std::vector<Zid>> g_pic_shown;  /* showing one: changed in place when it changes */
std::map<std::string, PixP> g_pics;
std::set<std::string> g_pic_none;
std::map<std::string, int> g_pic_tries;
/* First in, first out (the chat list's rows are made before the people and status pages' hundreds), and a jid wanted
 * again while waiting goes to the front: a row made again, or the chat's header, is on screen now (QA: the chat
 * header showed the letter while the photo waited behind a thousand contacts, fetched last-in first). */
std::deque<std::string> g_pic_queue;
int g_pic_busy; /* fetches under way, at most PIC_PARALLEL */
constexpr int PIC_PARALLEL = 3;
int g_pic_retry_timer;

void pic_prune(std::vector<Zid>& v) { v.erase(std::remove_if(v.begin(), v.end(), [](Zid z) { return !Zelement_is_valid(z); }), v.end()); }

void pictures_next();

std::set<std::string> g_pic_fetching;

void pic_queue(const std::string& jid) {
	if (g_pic_fetching.count(jid)) return;
	auto it = std::find(g_pic_queue.begin(), g_pic_queue.end(), jid);
	if (it == g_pic_queue.end()) {
		g_pic_queue.push_back(jid);
	} else if (it != g_pic_queue.begin()) {
		g_pic_queue.erase(it);
		g_pic_queue.push_front(jid);
	}
}

/* tried again after 2, 4, 8 .. 60 s while something still waits for it */
void pic_retry_later(const std::string& jid) {
	const int n = ++g_pic_tries[jid];
	const int ms = std::min(60000, 1000 << std::min(n, 6));
	after(ms, [jid] {
		if (g_pics.count(jid) || g_pic_none.count(jid)) return;
		auto it = g_pic_wanted.find(jid);
		if (it == g_pic_wanted.end()) return;
		pic_prune(it->second);
		if (it->second.empty()) return;
		pic_queue(jid);
		pictures_next();
	});
}

void pictures_next() {
	if (g_pic_busy >= PIC_PARALLEL || g_pic_queue.empty()) return;
	if (!engine_up()) {
		/* the engine comes up after the first rows: look again shortly */
		if (!g_pic_retry_timer) g_pic_retry_timer = after(1500, [] { g_pic_retry_timer = 0; pictures_next(); });
		return;
	}
	const std::string jid = g_pic_queue.front();
	g_pic_queue.pop_front();
	g_pic_busy++;
	g_pic_fetching.insert(jid);
	request("profile_picture", {{"jid", jid}, {"preview", true}}, [jid](const json* r, const char* err) {
		const std::string path = err ? "" : jstr(*r, "path");
		if (path.empty()) {
			g_pic_busy--;
			g_pic_fetching.erase(jid);
			if (err && !strcmp(err, "not_found") && !g_pics.count(jid)) g_pic_none.insert(jid);
			else if (!g_pics.count(jid)) pic_retry_later(jid);
			pictures_next();
			return;
		}
		image_file(path, 58, 58, true, [jid](PixP p) {
			g_pic_busy--;
			g_pic_fetching.erase(jid);
			if (p) {
				g_pics[jid] = p;
				g_pic_tries.erase(jid);
				auto& shown = g_pic_shown[jid];
				pic_prune(shown);
				for (const Zid img : shown) image_set(img, p);
				for (const Zid img : g_pic_wanted[jid]) {
					image_set(img, p);
					if (Zelement_is_valid(img)) shown.push_back(img);
				}
				g_pic_wanted.erase(jid);
			} else if (!g_pics.count(jid)) {
				pic_retry_later(jid);
			}
			pictures_next();
		});
	});
	pictures_next(); /* the next ones, up to PIC_PARALLEL at a time */
}

}  // namespace

bool unmetered() {
	/* the interface of the default route; looked up again at most every 10 s */
	static long checked = -100000;
	static bool b_ok = true;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const long now = long(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
	if (now - checked < 10000) return b_ok;
	checked = now;
	FILE* f = fopen("/proc/net/route", "r");
	if (!f) return b_ok = true;
	char line[256];
	bool b_any = false, b_cheap = false;
	while (fgets(line, sizeof line, f)) {
		char ifn[64];
		unsigned dest = 1, flags = 0;
		if (sscanf(line, "%63s %x %*x %x", ifn, &dest, &flags) != 3 || dest != 0 || !(flags & 1)) continue;
		b_any = true;
		if (!strncmp(ifn, "wl", 2) || !strncmp(ifn, "usb", 3) || !strncmp(ifn, "rndis", 5) || !strncmp(ifn, "eth", 3) || !strncmp(ifn, "en", 2)) b_cheap = true;
	}
	fclose(f);
	b_ok = b_cheap || !b_any;
	return b_ok;
}

float ui_scale() {
	static float s = [] {
		const char* e = getenv("ZRNAPP_SCALE");
		const float v = e ? float(atof(e)) : 0.0f;
		return v > 0 ? v : 4.0f; /* the S9: 1440 device px over the 360 px grid */
	}();
	return s;
}

std::vector<uint8_t> unbase64(const std::string& in) {
	std::vector<uint8_t> out;
	unsigned v = 0;
	int bits = 0;
	for (const unsigned char c : in) {
		const int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
		if (d < 0) continue;
		v = (v << 6) | unsigned(d);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(uint8_t(v >> bits));
		}
	}
	return out;
}

void image_file(const std::string& path, const float max_w, const float max_h, const bool b_cover, std::function<void(PixP)> done) {
	/* the file's time is in the key: a profile picture is saved again at the same path when it changes */
	struct stat st {};
	stat(path.c_str(), &st);
	char key[96];
	snprintf(key, sizeof key, "|%.0fx%.0f%c|%lld.%ld", double(max_w), double(max_h), b_cover ? 'c' : 'f', (long long)st.st_mtim.tv_sec, long(st.st_mtim.tv_nsec));
	const std::string k = path + key;
	if (PixP p = cache_get(k)) {
		done(p);
		return;
	}
	work([path, max_w, max_h, b_cover, k, done = std::move(done)]() mutable {
		PixP p = decode(read_all(path), max_w, max_h, b_cover);
		cache_put(k, p);
		post_idle([p, done = std::move(done)] { done(p); });
	});
}

/* thumbs: small JPEGs, shown blurred at the box's size until the full picture swaps in */
void image_bytes(std::vector<uint8_t> bytes, const float w, const float h, std::function<void(PixP)> done) {
	work([bytes = std::move(bytes), w, h, done = std::move(done)]() mutable {
		PixP p;
		if (bytes.size() > 12) {
			std::shared_ptr<Pix> raw;
			if (bytes[0] == 0xFF && bytes[1] == 0xD8) raw = jpeg(bytes, 0, 0);
			else if (bytes[0] == 0x89) raw = png(bytes);
			if (raw) p = resize(raw, std::max(1, int(w)), std::max(1, int(h)));
		}
		post_idle([p, done = std::move(done)] { done(p); });
	});
}

void image_set(const Zid image, const PixP& p) {
	if (!p || !Zelement_is_valid(image)) return;
	Zimage_set_source_rgba(image, p->w, p->h, p->px.data());
	Zelement_set_visibility(image, Z_VISIBLE);
}

void picture_want(const std::string& jid, const Zid image) {
	/* channels and broadcasts have none this way (the engine says so at once; asked, it waited 30 s for an answer) */
	if (jid.find("@newsletter") != std::string::npos || jid.find("@broadcast") != std::string::npos) return;
	auto it = g_pics.find(jid);
	if (it != g_pics.end()) {
		image_set(image, it->second);
		auto& shown = g_pic_shown[jid];
		if (shown.size() > 32) pic_prune(shown);
		shown.push_back(image);
		return;
	}
	if (g_pic_none.count(jid)) return;
	auto& v = g_pic_wanted[jid];
	if (v.size() > 32) pic_prune(v);
	v.push_back(image);
	pic_queue(jid);
	post_idle([] { pictures_next(); });
}

/* the contact changed it: fetched again; the old one stays on screen until the new one is in */
void picture_changed(const std::string& jid) {
	g_pic_none.erase(jid);
	g_pic_tries.erase(jid);
	pic_queue(jid);
	pictures_next();
}

/* QR: black on white in both themes, one bitmap pixel a module, a quiet zone of 4 (the painter samples NEAREST) */
PixP qr_pix(const std::string& text, int* const modules) {
	const qr::Code c = qr::encode(text);
	if (!c.n) return nullptr;
	const int quiet = 4, side = c.n + 2 * quiet;
	auto p = std::make_shared<Pix>();
	p->w = p->h = side;
	p->px.assign(size_t(side) * size_t(side) * 4, 255);
	for (int y = 0; y < c.n; y++)
		for (int x = 0; x < c.n; x++)
			if (c.at(x, y)) {
				uint8_t* o = &p->px[(size_t(y + quiet) * size_t(side) + size_t(x + quiet)) * 4];
				o[0] = o[1] = o[2] = 0;
			}
	*modules = side;
	return p;
}

std::string stickers_dir() {
	const char* home = getenv("HOME");
	std::string d = std::string(home ? home : "") + "/.local/share/zrn-whatsapp";
	mkdir(d.c_str(), 0700);
	d += "/stickers";
	mkdir(d.c_str(), 0700);
	return d;
}

}  // namespace wa
