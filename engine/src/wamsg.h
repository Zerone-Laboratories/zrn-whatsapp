// Decodes the few parts of WhatsApp's Message protobuf that the daemon shows, and
// builds the few it sends. Field numbers are protocol facts.
#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "crypto.h"
#include "pb.h"

namespace zw {

using nlohmann::json;

struct ParsedMsg {
    std::string kind;  // empty means no displayable content
    std::string text, quotedId;
    json media;   // the public part of IPC.md, or null
    json secret;  // media keys, kept only in the sealed store row
    // Side channels carried in the same message.
    std::string skdmGroup;
    Bytes skdm;
    std::string dsmDest;
    bool isProtocol = false;
    uint64_t protoType = 0;
    std::string targetId;  // revoke, edit or reaction target
    std::string editText;
    std::optional<pb::Msg> history;  // HistorySyncNotification
    std::vector<std::pair<Bytes, Bytes>> appKeys;  // app state key id, key data
    // WebMessageInfo bytes the phone sent back for a placeholder resend request (peer data operation 4).
    std::vector<Bytes> resent;
    // The shape of a peer data operation response (field numbers and sizes, never content), for the log.
    std::string pdoShape;
};

// Throws on a malformed protobuf.
ParsedMsg parseMessage(const Bytes& proto);
std::string previewOf(const std::string& kind, const std::string& text);

Bytes buildTextMessage(const std::string& text, const std::string& quotedId, const std::string& quotedSender);
Bytes buildRevoke(const std::string& chat, const std::string& id);
Bytes buildEdit(const std::string& chat, const std::string& id, const std::string& text, int64_t tsMs);
Bytes buildSticker(const std::string& url, const std::string& direct, const Bytes& sha, const Bytes& enc,
                   const Bytes& key, uint64_t size, int w, int h, int64_t ts);
// What an upload gives, plus what the message says about the file.
struct MediaOut {
    std::string url, direct, mime, fileName, caption, quotedId, quotedSender;
    Bytes sha, enc, key, thumb, waveform;
    uint64_t size = 0;
    int width = 0, height = 0, seconds = 0;
    bool ptt = false, animated = false;
    int64_t ts = 0;
};
// kind: image, audio, document or sticker.
Bytes buildMediaMessage(const std::string& kind, const MediaOut& o);
// The HKDF info of a kind's media keys, and the mms path segment of such keys.
const char* mediaKeysInfo(const std::string& kind);
std::string mmsTypeOf(const std::string& keysInfo);
Bytes buildStatusText(const std::string& text);
Bytes buildDeviceSent(const std::string& dest, const Bytes& inner);
Bytes buildSkdmMessage(const std::string& group, const Bytes& skdm);
// Asks our own phone for up to `count` messages of a chat older than the one given (peer data operation
// HISTORY_SYNC_ON_DEMAND); they come back as a history sync notification of their own.
Bytes buildHistoryOnDemand(const std::string& chat, const std::string& oldestId, bool oldestFromMe, int64_t oldestTsMs, int count);
// Asks our own phone to send a stored message again in full (peer data operation PLACEHOLDER_MESSAGE_RESEND): how a
// message stored without its media keys gets them. participant is the sender in a group, else empty.
// chatAlt: the chat's other address (its private id for a phone number, or the other way), asked for as well: the
// phone looks the message up under the address it keeps it by. Empty when there is none.
Bytes buildPlaceholderResend(const std::string& chat, bool fromMe, const std::string& id, const std::string& participant,
                             const std::string& chatAlt = "");

}  // namespace zw
