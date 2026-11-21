# WhatsApp for ztOS

**Requires ztOS BETA-1.1 or later (version 0.2.1-BETA-1.1).** ztOS applications do not run on earlier versions.

**This is an unofficial WhatsApp client.** It is not made or approved by WhatsApp, and is not affiliated with WhatsApp or Meta. The WhatsApp name and logo belong to their owner.
WhatsApp's terms do not allow third-party clients, and a number can be banned for
using one. Link it only if you accept that.

It runs as a linked device of the phone that owns the account. Calls are not
supported. It draws with libzrnui in the system's style, Metro or DotMatrix.

## What is in it

| | |
|---|---|
| `ui/` | the app (C++, libzrnui) |
| `engine/` | the protocol engine, `ztwhatsappd` |
| `notify/` | the notifier |
| `IPC.md` | how they talk |
| `third_party/nlohmann` | JSON, MIT, at build time |

A `.zexec` holds one program, so one binary has three roles: the app, `--daemon`
and `--notifier`. The app starts the other two, detached, when it cannot reach
the engine, and they stay after the window closes. The first time the engine
runs from the installed package it writes two systemd user units,
`~/.config/systemd/user/zrn-ztwhatsappd.service` and
`zrn-ztwhatsapp-notify.service`, and enables them for `default.target`: from
then on both start at login and restart on failure, so messages arrive after a
reboot without opening the app. Locks keep a second engine (exit 3) and a
second notifier (exit 0) out, whoever starts them.

### Background and power

- The engine blocks in epoll. Its only timer while connected is the keepalive
  ping, every 20 to 30 s of awake time; its clock stops in SoC sleep and never
  wakes the phone.
- A new address on any interface (netlink) or a return from SoC sleep
  (`wakes=` in `/run/zrn-sleepd/status`, watched with inotify) makes it ping at
  once and replace a silent socket, skipping the backoff.
- Offline, it retries with backoff from 2 s up to 2 min.
- The notifier holds a short Sleep1 inhibit after a long sleep, so the engine
  can reconnect and the backlog arrive; it reports `paired=1` so zrn-sleepd
  uses short doze windows. Missed messages come as notifications once,
  above the saved watermark.

## Removing it

Removing the app stops the engine and notifier and deletes its messages and keys from
this phone. It does not unlink: remove the device under Linked devices on your phone too.
Once the package is gone the login units no longer start (they need the .zexec);
`zrn-exec ~/.local/share/zrn/apps/zrn-whatsapp/zrn-whatsapp.zexec --daemon --remove-units`
before removing it deletes them as well.

## Licences

GPL-2.0-or-later, see `COPYING`. `engine/NOTICE.md` lists what the engine read as
protocol reference: `engine/src/tokens.inc` carries MPL-2.0. `engine/tests/gofix`,
a test-only tool that links GPL-3.0 code, is not in this repository.
Segoe UI is not included; the phone's system font is used.

## The UI (zrnui v2)

`ui/` is the app on zrn-sdk v2 (zrnui + zrnapp), built to `docs/UI-SPEC.md`: one file a screen group
(`page_main.cpp`, `page_chat.cpp`, `page_other.cpp`), `io.cpp` (engine socket, timers, decoding thread),
`look.cpp` (theme-bound controls and glyphs), `media.cpp` (pictures), `gaps.h` (the spec's SDK gaps, used when the
SDK has them). Build with CMake:

    cmake --preset host-debug && cmake --build --preset host-debug       # zrn-sdk in ../zrn-sdk (ZRN_SDK_DIR)
    cmake --preset phone-release && cmake --build --preset phone-release # aarch64, sysroot from zrn-sdk/tools/zrn

`ZWA_SELFTEST_DIR=dir` runs the UI without a window against the socket in `ZTWHATSAPP_SOCK` (use
`zrn-whatsapp --daemon --fake --socket PATH`) and writes PNG frames there.
