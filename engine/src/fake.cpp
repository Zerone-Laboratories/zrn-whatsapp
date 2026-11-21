// `ztwhatsappd --fake`: the IPC contract with invented data and no network or disk.
// It exists so the UI and notifier can be built without a real account.
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <zlib.h>

#include <cmath>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <map>

#include "engine.h"

namespace zw {

namespace {
// An invented picture: a gradient PNG with a hue chosen by the caller.
std::string writePng(const std::string& name, int w, int h, int hue) {
    const char* run = getenv("XDG_RUNTIME_DIR");
    std::string dir = std::string(run ? run : "/tmp") + "/ztwhatsapp-fake";
    mkdir(dir.c_str(), 0700);
    std::string path = dir + "/" + name + ".png";
    Bytes raw;
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        for (int x = 0; x < w; x++) {
            raw.push_back((uint8_t)(hue == 0 ? 40 + x * 200 / w : 30));
            raw.push_back((uint8_t)(hue == 1 ? 40 + x * 200 / w : 60 + y * 120 / h));
            raw.push_back((uint8_t)(hue == 2 ? 40 + x * 200 / w : 90));
        }
    }
    uLongf zn = compressBound(raw.size());
    Bytes z(zn);
    compress(z.data(), &zn, raw.data(), raw.size());
    z.resize(zn);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "";
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    auto chunk = [&](const char* t, const Bytes& d) {
        Bytes all(t, t + 4);
        append(all, d);
        uint32_t n = (uint32_t)d.size(), c = (uint32_t)crc32(0, all.data(), (uInt)all.size());
        uint8_t l[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
        uint8_t k[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
        fwrite(l, 1, 4, f);
        fwrite(all.data(), 1, all.size(), f);
        fwrite(k, 1, 4, f);
    };
    Bytes ihdr = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                  (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
    return path;
}

std::string fakeDir() {
    const char* run = getenv("XDG_RUNTIME_DIR");
    std::string dir = std::string(run ? run : "/tmp") + "/ztwhatsapp-fake";
    mkdir(dir.c_str(), 0700);
    return dir;
}

// An invented voice note: a WAV of rising beeps (the UI plays whatever ffmpeg reads, whatever the mime says).
std::string writeVoice(int seconds) {
    std::string path = fakeDir() + "/voice.wav";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "";
    const uint32_t rate = 16000, n = rate * (uint32_t)seconds, bytes = n * 2;
    auto u32 = [&](uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; fwrite(b, 1, 4, f); };
    auto u16 = [&](uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; fwrite(b, 1, 2, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(bytes);
    double ph = 0;
    for (uint32_t i = 0; i < n; i++) {
        double t = (double)i / rate, hz = 300 + 200 * (int)t;
        bool on = fmod(t, 1.0) < 0.6;
        ph += 2 * M_PI * hz / rate;
        int16_t v = on ? (int16_t)(8000 * sin(ph)) : 0;
        u16((uint16_t)v);
    }
    fclose(f);
    return path;
}

std::string writeText(const std::string& name, const std::string& text) {
    std::string path = fakeDir() + "/" + name;
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
    return path;
}

// A sticker: the first saved one, else a picture standing in for it.
std::string stickerFile() {
    const char* home = getenv("HOME");
    std::string dir = std::string(home ? home : "") + "/.local/share/zrn-whatsapp/stickers";
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 5 && n.substr(n.size() - 5) == ".webp") {
                closedir(d);
                return dir + "/" + n;
            }
        }
        closedir(d);
    }
    return writePng("sticker", 256, 256, 2);
}

class FakeEngine : public Engine {
public:
    FakeEngine(Loop& l, IpcServer& ipc) : loop_(l), ipc_(ipc) {
        add("61400000001@s.whatsapp.net", "Sam Example", false);
        add("61400000002@s.whatsapp.net", "Alex Test", false);
        add("120363000000000001@g.us", "Family (fake)", true);
        int64_t t = time(nullptr) - 7200;
        for (auto& [id, c] : chats_)
            for (int i = 0; i < 80; i++) {
                bool me = i % 3 == 0;
                std::string sender = me ? kSelf : (c["is_group"].get<bool>() ? "61400000001@s.whatsapp.net" : id);
                push(id, sender, me, "Fake message " + std::to_string(i + 1), t + i * 60, false);
            }
        add("61400000003@s.whatsapp.net", "Old Archive", false);
        chats_["61400000003@s.whatsapp.net"]["archived"] = true;
        chats_["61400000002@s.whatsapp.net"]["muted"] = true;
        push("61400000003@s.whatsapp.net", "61400000003@s.whatsapp.net", false, "An archived chat", t, false);
        // One photo, a starred message, for the pages that need them.
        std::string pid = push("61400000001@s.whatsapp.net", "61400000001@s.whatsapp.net", false, "A fake photo", t + 7000, false);
        json& pm = msgs_["61400000001@s.whatsapp.net"].back();
        pm["kind"] = "image";
        pm["media"] = {{"mime", "image/png"}, {"size", 2048}, {"width", 320}, {"height", 240}, {"file_name", ""}, {"thumb_b64", ""}, {"path", ""}};
        imageId_ = pid;
        // A voice note each way, a document and a sticker, for the media bubbles.
        const std::string sam = "61400000001@s.whatsapp.net";
        media(sam, sam, false, "audio", t + 7060, {{"mime", "audio/ogg; codecs=opus"}, {"size", 9000}, {"seconds", 4}, {"ptt", true},
              {"waveform", "MjI1OT1CRUhJSEQ+Ny0kGhELBgUGCxIbJTA6Q0pPUVBNSUQ+ODQxLzAyNTk8P0FAPjozLCMbEw0JBwkNFB4oMw=="}});
        media(sam, kSelf, true, "audio", t + 7080, {{"mime", "audio/ogg; codecs=opus"}, {"size", 9000}, {"seconds", 4}, {"ptt", true}});
        media(sam, sam, false, "document", t + 7100, {{"mime", "text/plain"}, {"size", 61}, {"file_name", "fake notes.txt"}});
        media(sam, sam, false, "sticker", t + 7120, {{"mime", "image/webp"}, {"size", 20000}, {"width", 512}, {"height", 512}});
        msgs_["61400000001@s.whatsapp.net"][5]["starred"] = true;
        starred_ = msgs_["61400000001@s.whatsapp.net"][5];
        statusItems_.push_back(statusItem("FAKES1", "61400000001@s.whatsapp.net", "Sam Example", "A fake status", 600));
        statusItems_.push_back(statusItem("FAKES2", "61400000002@s.whatsapp.net", "Alex Test", "Another fake status", 1800));
        timer();
    }

    std::vector<json> greeting() override { return {connection(), pairing()}; }

    void command(const std::string& cmd, const json& r, Reply rp) override {
        if (cmd == "status") {
            rp.ok({{"ipc_version", kIpcVersion}, {"connection", connection()}, {"pairing", pairing()},
                   {"self", paired_ ? kSelf : ""}, {"push_name", paired_ ? "Fake Me" : ""}});
        } else if (cmd == "pair_qr") {
            rp.ok(json::object());
            state_ = "qr";
            ipc_.broadcast(pairing());
            // Pretend the phone scans after a short wait.
            loop_.after(8000, [this] { finish(); });
        } else if (cmd == "pair_phone") {
            code_ = "ABCD-EFGH";
            state_ = "phone_code";
            rp.ok({{"code", code_}});
            ipc_.broadcast(pairing());
            loop_.after(8000, [this] { finish(); });
        } else if (cmd == "pair_cancel") {
            state_ = paired_ ? "paired" : "unpaired";
            rp.ok(json::object());
            ipc_.broadcast(pairing());
        } else if (cmd == "logout") {
            paired_ = false;
            state_ = "unpaired";
            rp.ok(json::object());
            ipc_.broadcast(connection());
            ipc_.broadcast(pairing());
        } else if (cmd == "chats") {
            int limit = r.value("limit", 100), off = r.value("offset", 0), i = 0;
            json out = json::array();
            for (auto& [id, c] : chats_)
                if (i++ >= off && (int)out.size() < limit) out.push_back(c);
            rp.ok({{"chats", out}, {"total", chats_.size()}});
        } else if (cmd == "messages") {
            auto it = msgs_.find(r.at("chat").get<std::string>());
            if (it == msgs_.end()) return rp.err("not_found", "no such chat");
            int limit = r.value("limit", 50);
            int64_t before = r.value("before_ts", (int64_t)INT64_MAX);
            json page = json::array();
            bool more = false;
            auto& v = it->second;
            for (int i = (int)v.size() - 1; i >= 0; i--) {
                if (v[i]["timestamp"].get<int64_t>() >= before) continue;
                if ((int)page.size() == limit) {
                    more = true;
                    break;
                }
                page.insert(page.begin(), v[i]);
            }
            rp.ok({{"messages", page}, {"more", more}});
        } else if (cmd == "send_text") {
            std::string chat = r.at("chat").get<std::string>();
            if (!chats_.count(chat)) return rp.err("not_found", "no such chat");
            std::string id = push(chat, kSelf, true, r.at("text").get<std::string>(), time(nullptr), true);
            if (!r.value("quoted_id", "").empty()) msgs_[chat].back()["quoted_id"] = r["quoted_id"];
            rp.ok({{"id", id}, {"timestamp", (int64_t)time(nullptr)}});
            for (auto [ms, st] : {std::pair{500, "sent"}, {1500, "delivered"}, {4000, "read"}})
                loop_.after(ms, [this, chat, id, st = std::string(st)] {
                    ipc_.broadcast({{"event", "receipt"}, {"chat", chat}, {"ids", {id}}, {"status", st}, {"sender", ""}});
                });
        } else if (cmd == "mark_read") {
            std::string chat = r.at("chat").get<std::string>();
            int n = chats_.count(chat) ? chats_[chat]["unread"].get<int>() : 0;
            if (n) {
                chats_[chat]["unread"] = 0;
                ipc_.broadcast({{"event", "chat"}, {"chat", chats_[chat]}});
            }
            rp.ok({{"marked", n}});
        } else if (cmd == "download_media") {
            // Strict on purpose: this is what proves a client sends message_id.
            std::string mid = r.at("message_id").get<std::string>();
            if (mid == imageId_) return rp.ok({{"path", writePng("photo", 640, 480, 1)}});
            json* m = find(r.value("chat", ""), mid);
            if (!m || !(*m)["media"].is_object()) return rp.err("not_found", "no such message");
            std::string kind = (*m)["kind"], path = (*m)["media"].value("path", "");
            if (path.empty()) path = kind == "audio" ? writeVoice(4) : kind == "sticker" ? stickerFile()
                                   : kind == "document" ? writeText("fake notes.txt", "A fake document from the fake engine.\nNothing real in it.\n") : "";
            if (path.empty()) return rp.err("unsupported", "this kind cannot be downloaded");
            // As the real engine, a little later: the UI shows its progress meanwhile.
            loop_.after(700, [this, rp, chat = r.value("chat", ""), mid, path] {
                if (json* again = find(chat, mid)) (*again)["media"]["path"] = path;
                rp.ok({{"path", path}});
            });
        } else if (cmd == "send_media" || cmd == "send_sticker") {
            std::string chat = r.at("chat").get<std::string>(), path = r.at("path").get<std::string>();
            std::string kind = cmd == "send_sticker" ? "sticker" : r.value("kind", "document");
            if (!chats_.count(chat)) return rp.err("not_found", "no such chat");
            if (access(path.c_str(), R_OK) != 0) return rp.err("not_found", "the file cannot be read");
            struct stat sb{};
            stat(path.c_str(), &sb);
            std::string fname = r.value("file_name", path.substr(path.rfind('/') + 1));
            json md = {{"mime", r.value("mime", kind == "sticker" ? "image/webp" : "application/octet-stream")}, {"size", (int64_t)sb.st_size},
                       {"width", r.value("width", 0)}, {"height", r.value("height", 0)}, {"file_name", kind == "document" ? fname : ""},
                       {"thumb_b64", ""}, {"path", path}};
            if (kind == "audio") {
                md["seconds"] = r.value("seconds", 0);
                md["ptt"] = r.value("ptt", false);
                md["waveform"] = r.value("waveform", "");
            }
            std::string id = media(chat, kSelf, true, kind, time(nullptr), md, r.value("quoted_id", ""), true);
            rp.ok({{"id", id}, {"timestamp", (int64_t)time(nullptr)}});
            for (auto [ms, st] : {std::pair{1200, "sent"}, {2000, "delivered"}, {5000, "read"}})
                loop_.after(ms, [this, chat, id, st = std::string(st)] {
                    ipc_.broadcast({{"event", "receipt"}, {"chat", chat}, {"ids", {id}}, {"status", st}, {"sender", ""}});
                });
        } else if (cmd == "media_played") {
            json* m = find(r.value("chat", ""), r.value("message_id", ""));
            if (!m) return rp.err("not_found", "no such message");
            if ((*m)["media"].is_object()) (*m)["media"]["played"] = true;
            rp.ok(json::object());
        } else if (cmd == "statuses") {
            std::map<std::string, json> by;
            for (auto& it : statusItems_) {
                std::string who = it["sender"];
                if (!by.count(who)) by[who] = {{"sender", who}, {"name", it["sender_name"]}, {"last_ts", it["timestamp"]}, {"unseen", 0}, {"items", json::array()}};
                if (!it["seen"].get<bool>()) by[who]["unseen"] = by[who]["unseen"].get<int>() + 1;
                by[who]["items"].push_back(it);
            }
            json out = json::array();
            for (auto& [_, c] : by) out.push_back(c);
            rp.ok({{"contacts", out}});
        } else if (cmd == "status_seen") {
            for (auto& id : r.at("ids"))
                for (auto& it : statusItems_)
                    if (it["id"] == id) it["seen"] = true;
            rp.ok(json::object());
        } else if (cmd == "post_status") {
            std::string id = "FAKEPOST" + std::to_string(++seq_);
            json it = statusItem(id, kSelf, "You", r.at("text").get<std::string>(), 0);
            it["from_me"] = true;
            it["seen"] = true;
            statusItems_.push_back(it);
            rp.ok({{"id", id}, {"timestamp", (int64_t)time(nullptr)}});
            ipc_.broadcast({{"event", "status"}, {"sender", kSelf}, {"sender_name", "You"}, {"item", it}, {"live", false}});
        } else if (cmd == "contacts") {
            json c = json::array();
            c.push_back({{"jid", "61400000002@s.whatsapp.net"}, {"name", "Alex Test"}, {"push_name", "Alex"}, {"phone", "+61400000002"}});
            c.push_back({{"jid", "61400000001@s.whatsapp.net"}, {"name", "Sam Example"}, {"push_name", "Sam"}, {"phone", "+61400000001"}});
            rp.ok({{"contacts", c}, {"total", 2}});
        } else if (cmd == "profile_picture") {
            std::string j = r.at("jid").get<std::string>();
            if (j == "61400000002@s.whatsapp.net") return rp.err("not_found", "no picture");  // privacy case
            rp.ok({{"path", writePng("pic-" + j.substr(9, 2), 96, 96, j[10] % 3)}});
        } else if (cmd == "group_info") {
            rp.ok({{"id", r.at("chat")}, {"subject", "Family (fake)"}, {"topic", "A fake group"}, {"owner", kSelf}, {"created", 1700000000},
                   {"announce", false}, {"locked", false},
                   {"participants", {{{"jid", kSelf}, {"name", "You"}, {"is_admin", true}, {"is_super_admin", true}},
                                     {{"jid", "61400000001@s.whatsapp.net"}, {"name", "Sam Example"}, {"is_admin", false}, {"is_super_admin", false}}}}});
        } else if (cmd == "search") {
            std::string q = r.at("query").get<std::string>();
            json hits = json::array();
            for (auto& [_, v] : msgs_)
                for (auto& m : v)
                    if (hits.size() < 50 && m["text"].get<std::string>().find(q) != std::string::npos) hits.push_back(m);
            rp.ok({{"messages", hits}, {"more", false}});
        } else if (cmd == "starred") {
            rp.ok({{"messages", {starred_}}});
        } else if (cmd == "typing" || cmd == "set_presence" || cmd == "subscribe_presence") {
            rp.ok(json::object());
        } else {
            rp.err("unknown_cmd", "unknown command");
        }
    }

private:
    static constexpr const char* kSelf = "61400000000@s.whatsapp.net";

    json connection() {
        return {{"event", "connection"}, {"state", paired_ ? "connected" : "offline"}, {"detail", "fake"}};
    }
    json pairing() {
        json j = {{"event", "pairing"}, {"state", state_}, {"qr", ""}, {"expires_in", 0}, {"code", ""}, {"error", ""}};
        if (state_ == "qr") {
            j["qr"] = "https://example.invalid/ztwhatsapp-fake-qr";
            j["expires_in"] = 20;
        }
        if (state_ == "phone_code") j["code"] = code_;
        if (state_ == "paired") j["self"] = kSelf;
        return j;
    }
    void finish() {
        if (state_ != "qr" && state_ != "phone_code") return;
        paired_ = true;
        state_ = "paired";
        ipc_.broadcast(pairing());
        ipc_.broadcast(connection());
        ipc_.broadcast({{"event", "history_sync"}, {"state", "done"}, {"progress", 100}, {"chats", chats_.size()}, {"messages", 240}});
    }
    void add(const std::string& id, const std::string& name, bool group) {
        chats_[id] = {{"id", id}, {"name", name}, {"is_group", group}, {"unread", 0}, {"last_text", ""},
                      {"last_ts", 0}, {"last_from_me", false}, {"muted", false}, {"pinned", false}, {"archived", false}};
    }
    std::string push(const std::string& chat, const std::string& sender, bool me, const std::string& text, int64_t ts, bool live) {
        std::string id = "FAKE" + std::to_string(++seq_);
        auto& c = chats_[chat];
        std::string sname = me ? "You" : (sender == chat ? c["name"].get<std::string>() : "Sam Example");
        json m = {{"chat", chat}, {"id", id}, {"from_me", me}, {"sender", sender}, {"sender_name", sname},
                  {"timestamp", ts}, {"kind", "text"}, {"text", text}, {"status", me ? (live ? "pending" : "read") : "received"},
                  {"quoted_id", ""}, {"media", nullptr}};
        msgs_[chat].push_back(m);
        c["last_text"] = text;
        c["last_ts"] = ts;
        c["last_from_me"] = me;
        if (live) {
            if (!me) c["unread"] = c["unread"].get<int>() + 1;
            ipc_.broadcast({{"event", "message"}, {"chat", chat}, {"chat_name", c["name"]}, {"sender", sender},
                            {"sender_name", sname}, {"text", text}, {"timestamp", ts}, {"id", id}, {"from_me", me},
                            {"is_group", c["is_group"]}, {"muted", false}, {"live", true}, {"message", m}});
            ipc_.broadcast({{"event", "chat"}, {"chat", c}});
        }
        return id;
    }
    json* find(const std::string& chat, const std::string& id) {
        auto it = msgs_.find(chat);
        if (it == msgs_.end()) return nullptr;
        for (auto& m : it->second)
            if (m["id"] == id) return &m;
        return nullptr;
    }
    // A media message; `live` announces it as the real engine does a send.
    std::string media(const std::string& chat, const std::string& sender, bool me, const std::string& kind, int64_t ts, json md,
                      const std::string& quoted = "", bool live = false) {
        for (const char* k : {"mime", "file_name", "thumb_b64", "path"})
            if (!md.contains(k)) md[k] = "";
        for (const char* k : {"size", "width", "height"})
            if (!md.contains(k)) md[k] = 0;
        std::string id = push(chat, sender, me, "", ts, false);
        json& m = msgs_[chat].back();
        m["kind"] = kind;
        m["media"] = md;
        m["quoted_id"] = quoted;
        if (me && live) m["status"] = "pending";
        static const std::map<std::string, std::string> label = {{"audio", "Voice message"}, {"document", "Document"}, {"sticker", "Sticker"}, {"image", "Photo"}};
        chats_[chat]["last_text"] = label.count(kind) ? label.at(kind) : kind;
        if (live) {
            auto& c = chats_[chat];
            ipc_.broadcast({{"event", "message"}, {"chat", chat}, {"chat_name", c["name"]}, {"sender", sender}, {"sender_name", m["sender_name"]},
                            {"text", c["last_text"]}, {"timestamp", ts}, {"id", id}, {"from_me", me}, {"is_group", c["is_group"]},
                            {"muted", false}, {"live", true}, {"message", m}});
            ipc_.broadcast({{"event", "chat"}, {"chat", c}});
        }
        return id;
    }
    void timer() {
        loop_.after(20000, [this] {
            if (paired_) {
                auto it = chats_.begin();
                std::advance(it, tick_++ % chats_.size());
                bool g = it->second["is_group"].get<bool>();
                push(it->first, g ? "61400000001@s.whatsapp.net" : it->first, false,
                     "Scripted fake message " + std::to_string(tick_), time(nullptr), true);
            }
            timer();
        });
    }

    Loop& loop_;
    IpcServer& ipc_;
    static json statusItem(const std::string& id, const std::string& who, const std::string& name, const std::string& text, int age) {
        return {{"chat", "status@broadcast"}, {"id", id}, {"from_me", false}, {"sender", who}, {"sender_name", name},
                {"timestamp", (int64_t)time(nullptr) - age}, {"kind", "text"}, {"text", text}, {"status", "received"},
                {"quoted_id", ""}, {"media", nullptr}, {"seen", false}};
    }
    bool paired_ = false;
    std::string imageId_;
    json starred_;
    std::vector<json> statusItems_;
    std::string state_ = "unpaired", code_;
    std::map<std::string, json> chats_;
    std::map<std::string, std::vector<json>> msgs_;
    int seq_ = 0, tick_ = 0;
};
}  // namespace

std::unique_ptr<Engine> makeFakeEngine(Loop& l, IpcServer& ipc) { return std::make_unique<FakeEngine>(l, ipc); }

}  // namespace zw
