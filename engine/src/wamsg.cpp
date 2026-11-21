#include "wamsg.h"

#include <functional>

namespace zw {

namespace {
std::string quotedOf(const pb::Msg& m, int ctxField) {
    if (auto c = m.sub(ctxField)) return c->str(1);
    return "";
}

void mediaInto(ParsedMsg& p, const pb::Msg& m, const char* kind, const char* mmsKind, int mime, int size, int w,
               int h, int thumb, int key, int fileSha, int enc, int path, int caption, int ctx, int fname) {
    p.kind = kind;
    if (caption) p.text = m.str(caption);
    p.quotedId = quotedOf(m, ctx);
    json j = {{"mime", m.str(mime)}, {"size", m.num(size)}, {"width", w ? m.num(w) : 0}, {"height", h ? m.num(h) : 0},
              {"file_name", fname ? m.str(fname) : ""}, {"thumb_b64", ""}, {"path", ""}};
    if (thumb && m.has(thumb)) {
        Bytes t = m.bytes(thumb);
        if (t.size() <= 64 * 1024) j["thumb_b64"] = b64(t);
    }
    p.media = j;
    if (m.has(key))
        p.secret = {{"type", mmsKind}, {"key", b64(m.bytes(key))}, {"sha", b64(m.bytes(fileSha))},
                    {"enc", b64(m.bytes(enc))}, {"path", m.str(path)}, {"url", m.str(1)}};
}
}  // namespace

ParsedMsg parseMessage(const Bytes& proto) {
    ParsedMsg p;
    pb::Msg m(proto);
    // Wrappers that hold a whole Message. Depth is capped against hostile nesting.
    for (int depth = 0; depth < 6; depth++) {
        if (auto s = m.sub(2)) {  // sender key distribution can sit beside any content
            p.skdmGroup = s->str(1);
            p.skdm = s->bytes(2);
        }
        std::optional<pb::Msg> inner;
        if (auto d = m.sub(31)) {
            p.dsmDest = d->str(1);
            inner = d->sub(2);
        } else {
            for (int f : {40, 37, 55, 53, 58, 62, 67})
                if (auto w = m.sub(f)) {
                    inner = w->sub(1);
                    break;
                }
        }
        if (!inner) break;
        m = std::move(*inner);
    }
    if (auto pm = m.sub(12)) {
        p.isProtocol = true;
        p.protoType = pm->num(2);
        if (auto k = pm->sub(1)) p.targetId = k->str(3);
        if (auto h = pm->sub(6)) p.history = std::move(*h);
        if (auto share = pm->sub(7))
            for (auto& k : share->repeated(1)) {
                auto id = k.sub(1);
                auto data = k.sub(2);
                if (id && data && !id->bytes(1).empty() && !data->bytes(1).empty()) p.appKeys.push_back({id->bytes(1), data->bytes(1)});
            }
        // A peer data operation response (type 17): the full messages of a placeholder resend request (request type 4).
        if (auto r = pm->sub(17)) {
            // field numbers and sizes only: what came back when nothing could be used
            std::function<std::string(const pb::Msg&, int)> shape = [&](const pb::Msg& m, int depth) {
                std::string o;
                for (auto& f : m.fields) {
                    o += std::to_string(f.num);
                    if (f.wire == 0) o += "=" + std::to_string(f.v);
                    else if (f.wire == 2) {
                        o += ":" + std::to_string(f.d.size());
                        if (depth < 3) try {
                            pb::Msg inner(f.d);
                            if (!inner.fields.empty() && f.d.size() < 4096) o += "{" + shape(inner, depth + 1) + "}";
                        } catch (...) {}
                    }
                    o += " ";
                }
                return o;
            };
            try { p.pdoShape = shape(*r, 0); } catch (...) {}
        }
        if (auto r = pm->sub(17))
            if (r->num(1) == 4)
                for (auto& res : r->repeated(3))
                    if (auto ph = res.sub(4))
                        if (ph->has(1)) p.resent.push_back(ph->bytes(1));
        if (auto e = pm->sub(14)) {
            ParsedMsg ed = parseMessage(e->fields.empty() ? Bytes{} : pm->bytes(14));
            p.editText = ed.text;
        }
        return p;
    }
    if (m.has(1)) {
        p.kind = "text";
        p.text = m.str(1);
    } else if (auto e = m.sub(6)) {
        p.kind = "text";
        p.text = e->str(1);
        p.quotedId = quotedOf(*e, 17);
    } else if (auto i = m.sub(3)) {
        mediaInto(p, *i, "image", "WhatsApp Image Keys", 2, 5, 7, 6, 16, 8, 4, 9, 11, 3, 17, 0);
    } else if (auto v = m.sub(9)) {
        p.kind = "video";
        p.text = v->str(7);
        p.media = {{"mime", v->str(2)}, {"size", v->num(4)}, {"width", v->num(10)}, {"height", v->num(9)},
                   {"file_name", ""}, {"thumb_b64", v->has(16) ? b64(v->bytes(16)) : ""}, {"path", ""}};
    } else if (auto a = m.sub(8)) {
        mediaInto(p, *a, "audio", "WhatsApp Audio Keys", 2, 4, 0, 0, 0, 7, 3, 8, 9, 0, 17, 0);
        p.media["seconds"] = a->num(5);
        p.media["ptt"] = a->num(6) != 0;
        if (a->has(19)) {
            Bytes wf = a->bytes(19);
            if (wf.size() <= 256) p.media["waveform"] = b64(wf);
        }
    } else if (auto d = m.sub(7)) {
        mediaInto(p, *d, "document", "WhatsApp Document Keys", 2, 5, 0, 0, 16, 7, 4, 9, 10, 20, 17, 8);
        if (p.media["file_name"].get<std::string>().empty()) p.media["file_name"] = d->str(3);
        p.media["pages"] = d->num(6);
    } else if (auto s = m.sub(26)) {
        mediaInto(p, *s, "sticker", "WhatsApp Image Keys", 5, 9, 7, 6, 16, 4, 2, 3, 8, 0, 17, 0);
        p.media["animated"] = s->num(13) != 0;
    } else if (auto c = m.sub(4)) {
        p.kind = "contact";
        p.text = c->str(1);
    } else if (m.has(5)) {
        p.kind = "location";
    } else if (auto r = m.sub(46)) {
        p.kind = "reaction";
        p.text = r->str(2);
        if (auto k = r->sub(1)) p.targetId = k->str(3);
    } else {
        // Only side-channel fields such as context info or a sender key mean nothing to show.
        bool content = false;
        for (auto& f : m.fields)
            if (f.num != 2 && f.num != 35) content = true;
        if (content) {
            p.kind = "unsupported";
            p.text = "This message type is not supported yet";
        }
    }
    return p;
}

std::string previewOf(const std::string& kind, const std::string& text) {
    if (!text.empty() && kind != "unsupported" && kind != "undecryptable") return text.substr(0, 200);
    if (kind == "image") return "Photo";
    if (kind == "video") return "Video";
    if (kind == "audio") return "Voice message";
    if (kind == "document") return "Document";
    if (kind == "sticker") return "Sticker";
    if (kind == "location") return "Location";
    if (kind == "contact") return "Contact";
    return text;
}

Bytes buildTextMessage(const std::string& text, const std::string& quotedId, const std::string& quotedSender) {
    pb::Writer w;
    if (quotedId.empty()) {
        w.s(1, text);
        return w.out;
    }
    pb::Writer ctx;
    ctx.s(1, quotedId);
    if (!quotedSender.empty()) ctx.s(2, quotedSender);
    pb::Writer e;
    e.s(1, text).m(17, ctx);
    w.m(6, e);
    return w.out;
}

namespace {
pb::Writer ownKey(const std::string& chat, const std::string& id) {
    pb::Writer k;
    k.s(1, chat).u(2, 1).s(3, id);
    return k;
}
}  // namespace

Bytes buildHistoryOnDemand(const std::string& chat, const std::string& oldestId, bool oldestFromMe, int64_t oldestTsMs, int count) {
    pb::Writer h;
    h.s(1, chat).s(2, oldestId).u(3, oldestFromMe ? 1 : 0).u(4, (uint64_t)count).u(5, (uint64_t)oldestTsMs);
    pb::Writer op;
    op.u(1, 3).m(4, h);
    pb::Writer pm;
    pm.u(2, 16).m(16, op);
    pb::Writer w;
    w.m(12, pm);
    return w.out;
}

Bytes buildPlaceholderResend(const std::string& chat, bool fromMe, const std::string& id, const std::string& participant,
                             const std::string& chatAlt) {
    pb::Writer op;
    op.u(1, 4);
    for (const std::string* c : {&chat, &chatAlt}) {
        if (c->empty()) continue;
        pb::Writer k;
        k.s(1, *c).u(2, fromMe ? 1 : 0).s(3, id);
        if (!participant.empty()) k.s(4, participant);
        pb::Writer req;
        req.m(1, k);
        op.m(5, req);
    }
    pb::Writer pm;
    pm.u(2, 16).m(16, op);
    pb::Writer w;
    w.m(12, pm);
    return w.out;
}

Bytes buildRevoke(const std::string& chat, const std::string& id) {
    pb::Writer pm;
    pm.m(1, ownKey(chat, id)).u(2, 0);
    pb::Writer w;
    w.m(12, pm);
    return w.out;
}

Bytes buildEdit(const std::string& chat, const std::string& id, const std::string& text, int64_t tsMs) {
    pb::Writer inner;
    inner.s(1, text);
    pb::Writer pm;
    pm.m(1, ownKey(chat, id)).u(2, 14).m(14, inner).u(15, (uint64_t)tsMs);
    pb::Writer w;
    w.m(12, pm);
    return w.out;
}

Bytes buildSticker(const std::string& url, const std::string& direct, const Bytes& sha, const Bytes& enc,
                   const Bytes& key, uint64_t size, int w, int h, int64_t ts) {
    pb::Writer s;
    s.s(1, url).b(2, sha).b(3, enc).b(4, key).s(5, "image/webp").u(6, (uint64_t)h).u(7, (uint64_t)w);
    s.s(8, direct).u(9, size).u(10, (uint64_t)ts);
    pb::Writer m;
    m.m(26, s);
    return m.out;
}

Bytes buildMediaMessage(const std::string& kind, const MediaOut& o) {
    pb::Writer ctx;
    bool hasCtx = !o.quotedId.empty();
    if (hasCtx) {
        ctx.s(1, o.quotedId);
        if (!o.quotedSender.empty()) ctx.s(2, o.quotedSender);
    }
    pb::Writer x;
    pb::Writer m;
    if (kind == "image") {
        x.s(1, o.url).s(2, o.mime);
        if (!o.caption.empty()) x.s(3, o.caption);
        x.b(4, o.sha).u(5, o.size).u(6, (uint64_t)o.height).u(7, (uint64_t)o.width).b(8, o.key).b(9, o.enc).s(11, o.direct).u(12, (uint64_t)o.ts);
        if (!o.thumb.empty()) x.b(16, o.thumb);
        if (hasCtx) x.m(17, ctx);
        m.m(3, x);
    } else if (kind == "audio") {
        x.s(1, o.url).s(2, o.mime).b(3, o.sha).u(4, o.size).u(5, (uint64_t)o.seconds).u(6, o.ptt ? 1 : 0).b(7, o.key).b(8, o.enc).s(9, o.direct).u(10, (uint64_t)o.ts);
        if (hasCtx) x.m(17, ctx);
        if (!o.waveform.empty()) x.b(19, o.waveform);
        m.m(8, x);
    } else if (kind == "document") {
        x.s(1, o.url).s(2, o.mime).s(3, o.fileName).b(4, o.sha).u(5, o.size).b(7, o.key).s(8, o.fileName).b(9, o.enc).s(10, o.direct).u(11, (uint64_t)o.ts);
        if (!o.thumb.empty()) x.b(16, o.thumb);
        if (hasCtx) x.m(17, ctx);
        m.m(7, x);
    } else {  // sticker
        x.s(1, o.url).b(2, o.sha).b(3, o.enc).b(4, o.key).s(5, o.mime.empty() ? "image/webp" : o.mime).u(6, (uint64_t)o.height).u(7, (uint64_t)o.width);
        x.s(8, o.direct).u(9, o.size).u(10, (uint64_t)o.ts);
        if (o.animated) x.u(13, 1);
        if (hasCtx) x.m(17, ctx);
        m.m(26, x);
    }
    return m.out;
}

const char* mediaKeysInfo(const std::string& kind) {
    if (kind == "audio") return "WhatsApp Audio Keys";
    if (kind == "document") return "WhatsApp Document Keys";
    if (kind == "video") return "WhatsApp Video Keys";
    return "WhatsApp Image Keys";
}

std::string mmsTypeOf(const std::string& keysInfo) {
    if (keysInfo == "WhatsApp Audio Keys") return "audio";
    if (keysInfo == "WhatsApp Document Keys") return "document";
    if (keysInfo == "WhatsApp Video Keys") return "video";
    return "image";
}

Bytes buildStatusText(const std::string& text) {
    pb::Writer e;
    e.s(1, text).f32(7, 0xFFFFFFFFu).f32(8, 0xFF00796Bu).u(9, 0);  // white text on a plain background
    pb::Writer w;
    w.m(6, e);
    return w.out;
}

Bytes buildDeviceSent(const std::string& dest, const Bytes& inner) {
    pb::Writer d;
    d.s(1, dest).b(2, inner);
    pb::Writer w;
    w.m(31, d);
    return w.out;
}

Bytes buildSkdmMessage(const std::string& group, const Bytes& skdm) {
    pb::Writer s;
    s.s(1, group).b(2, skdm);
    pb::Writer w;
    w.m(2, s);
    return w.out;
}

}  // namespace zw
