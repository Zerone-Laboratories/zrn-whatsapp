# WhatsApp for ztOS

**This is an unofficial WhatsApp client.** It is not made or approved by WhatsApp.
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
the engine, and they stay after the window closes. **They do not start by
themselves after a reboot**: notifications begin once the app has been opened.

## Licences

GPL-2.0-or-later, see `COPYING`. `engine/NOTICE.md` lists what the engine read as
protocol reference: `engine/src/tokens.inc` carries MPL-2.0. `engine/tests/gofix`,
a test-only tool that links GPL-3.0 code, is not in this repository.
Segoe UI is not included; the phone's system font is used.
