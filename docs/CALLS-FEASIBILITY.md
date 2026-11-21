# Voice calls: feasibility

Status: investigation only, 2026-11-20. Nothing implemented. No account, session or
traffic of the owner was used. Evidence comes from public sources only. The WA Web
JS/WASM bundle was not yet analysed with REA (see Unknowns, milestone M0).

## Verdict

Feasible for 1:1 voice. A working open-source reference now exists: **meowcaller**
(Go, MIT, on top of whatsmeow) does inbound and outbound 1:1 audio and video against
real phones. Port its protocol to C++ rather than reverse-engineering from scratch.

The hard part is not signaling. It is the media leg: WhatsApp's own relay (TURN-like),
SRTP keying, and the **MLow** codec (proprietary, Opus fallback only partly done in
meowcaller). Plain libwebrtc does not drop in.

## 1. Protocol on the wire (what is known)

Signaling (binary XML over the existing Noise socket):
- `<call>` stanza with children: `offer`, `preaccept`, `accept`, `reject`,
  `transport`, `relaylatency`, `terminate`, plus `receipt`/`ack` per child.
- Offer/accept carry `<audio enc="opus" rate="16000"/>` and `rate="8000"`,
  `<net medium="3"/>`, `<encopt keygen="2"/>`, and relay data (tokens, relay
  endpoints, a relay key). Reported in whatsmeow #555.
- Call key: a random 32-byte key, sent per recipient device inside `<enc>` encrypted
  with the Signal session (same as a message). **The server silently drops an
  offer whose call key is a `pkmsg` (pre-key message)**; it works once a normal
  `msg` session exists (whatsmeow #555, abaz1997). Our sigproto already does both
  types, so we must establish a session first (e.g. a silent prior exchange).

Media:
- Relay servers `relay-*.facebook.com:3478`, STUN/TURN-style Allocate + Bind.
  Reported: the TURN HMAC-SHA1 key is the base64 relay-key string as ASCII, not
  the decoded bytes (#555, neoand, unverified).
- RTP over that relay, **payload type 120 for 1:1 audio whatever the codec**
  (meowcaller docs); codec chosen by signaling.
- SRTP keys derived by HKDF from the call key; salts from the offer's
  encryption payload (#555).
- Codec: current clients prefer **MLow** (Meta's low-bitrate codec). Opus 16 kHz
  is the fallback still negotiated in offers.
- Known trap: callee decrypts audio but the call drops ("Reconnecting") after
  ~20 s; suspected missing RTCP/keep-alive, DTX framing or a header extension
  (#555). meowcaller appears past this.

WebRTC in the browser: WA Web calls (launched 28 Jul 2026) are reported to run a
**WebAssembly VoIP engine** (#555, w3nder), i.e. WhatsApp's own stack compiled to
WASM, not the browser's RTCPeerConnection media path. So the wire format is
WhatsApp-native, not standard ICE/DTLS-SRTP. Unconfirmed by our own bundle
analysis.

## 2. Open source state

| Project | Status |
|---|---|
| purpshell/meowcaller (Go, MIT) | 1:1 audio+video in/out, MLow in pure Go, relay/stun/srtp/rtp packages; group calls experimental; Opus fallback in progress. Spec from the "WhatsApp Calls Research Group". |
| whatsapp-rust (PR #1024) | Independent video-transition implementation, cross-checked by meowcaller. |
| whatsmeow (tulir) | Parses/receives call events, can reject; no offer/media upstream. Issue #555 is the umbrella; maintainer silent on it. |
| Baileys | Receives call events, `rejectCall`; no media. |
| wazmeow notes | Integration guide; relay silent until answered. |
| Meta Business Calling API | Official, WebRTC/SDP, but business numbers only (WABA). Not usable for a personal account. |

## 3. Can a linked device call?

Yes. WA Web got 1:1 and group voice/video on 28 Jul 2026 (staged rollout); Desktop
had it before. Calls go to/from companion devices, so a linked device is
allowed at the protocol level. Rollout is server-gated per account, so the
server may still decline for a client it does not flag as call-capable. Our
client registers as a companion; which platform/version it claims probably
decides this.

## 4. Minimum viable architecture (1:1 voice)

Engine (C++, engine/src):
- `call.cpp/h`: state machine (idle, offering, ringing, preaccepted, active,
  ended), builds/parses `<call>` children via node.cpp, acks every child.
- Call key: 32 random bytes; encrypt per device via sigproto; ensure non-prekey
  session before offering.
- Relay: small STUN/TURN client (Allocate, Bind, ChannelData/Send), latency probe
  answering `relaylatency`.
- SRTP: libsrtp2 (AES-CM-128 + HMAC-SHA1-80 assumed; verify) with HKDF from crypto.cpp.
- RTP/RTCP: own minimal packetiser, PT 120, sequence/timestamp, keep-alive.
- Codec: libopus 16 kHz first (negotiate opus only); MLow later, porting
  meowcaller's Go decoder/encoder.
- Jitter buffer: simple adaptive buffer (60-200 ms) plus Opus PLC.

Audio I/O: PipeWire stream (16 kHz mono, 20 ms quantum), role `Communication`
so the S9 audio policy routes to earpiece/speaker.

Echo cancellation: WebRTC AEC3 via **webrtc-audio-processing** (the PipeWire
`echo-cancel` module already uses it). Cheapest path: load
`libpipewire-module-echo-cancel` and point the call stream at its virtual source.

IPC/UI: new IPC events `call.incoming/state/ended`, commands
`call.start/accept/reject/hangup/mute/speaker` (extend IPC.md); zrnui call
screen; ztnotifyd full-screen incoming call (lifts the "calls out of scope" note).

Power: hold a wake lock while ringing/active; incoming offers arrive over the
existing socket, so they only ring while the engine is connected (sleep wakeups
via pushd are a separate concern).

## 5. Risks

- **Account ban**: unofficial client placing calls is a new, visible signal; a
  malformed offer or odd codec set is easy to fingerprint. Test only on a
  throwaway number first.
- **Protocol churn**: Meta moved to MLow and may drop Opus fallback; offers may
  then fail with only Opus. Relay auth details can change silently.
- **Server gating**: rollout flags may refuse calls from our companion type.
- **Licensing**: meowcaller is MIT, fine to port with notice. MLow is
  proprietary; a clean-room port carries some legal uncertainty.
- **Phone**: CPU/thermal under AEC + codec on little cores, audio routing on
  ABOX (earpiece is the second MAX98512). Bluetooth headset later.

## 6. Effort (agent-days)

| Milestone | Days |
|---|---|
| M0 Read meowcaller + REA on WA Web bundle/WASM, write wire spec | 2 |
| M1 Signaling: receive offer, ring, reject, terminate | 2 |
| M2 Call key + outbound offer/accept to a test phone (rings) | 2 |
| M3 Relay client + SRTP + RTP, Opus, two-way audio on laptop | 5 |
| M4 PipeWire I/O, AEC, jitter buffer, call stability > 10 min | 3 |
| M5 IPC + zrnui call screen + notify ring + phone audio routing | 3 |
| M6 MLow codec port (if Opus fallback rejected) | 4-6 |
| Total | 17-23 |

## 7. Recommended plan

1. M0 first: port spec from meowcaller, confirm with REA on the public WA Web
   bundle (no login). Decide Opus vs MLow from what current phones accept.
2. M1-M3 on the laptop with two throwaway numbers; meowcaller as a known-good
   peer to diff packets against.
3. M4-M5, then phone tests under the owner's oversight.
4. Only after stable 1:1 voice: video or groups.

## Biggest unknowns

- Whether current phones still accept an Opus-only offer (else MLow is mandatory).
- Exact SRTP profile, HKDF info strings and the ~20 s drop cause.
- Whether the server lets our companion type call (rollout gating).
- Ban sensitivity of unofficial calling.

## Sources

- https://github.com/purpshell/meowcaller
- https://pkg.go.dev/github.com/purpshell/meowcaller
- https://github.com/tulir/whatsmeow/issues/555
- https://social.dev.setpar.com.br/docs/call-audio-implementation-guide.md
- https://macrumors.com/2026/07/29/whatsapp-web-calling-waiting-room-features
- https://9to5google.com/2026/07/28/whatsapp-web-calling-new-features/
- https://webrtchacks.com/whats-up-with-whatsapp-and-webrtc/
