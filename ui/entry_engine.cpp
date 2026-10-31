// SPDX-License-Identifier: GPL-2.0-or-later
// One binary holds the app, the engine and the notifier: a .zexec has one program.
#define main ztwhatsappd_main
#include "../engine/src/main.cpp"
#undef main
