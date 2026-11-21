// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The SDK gaps of docs/UI-SPEC.md section 8, behind small shims. The build (CMakeLists.txt) checks the zrnui
 * headers for each API and defines ZWA_HAVE_<gap>; without it the shim degrades as the spec allows
 * (interim tap for Holding, turnstile for continuum / slide-up, no keyboard inset, no bitmap cache, no anchoring
 * beyond what the app does itself). Rebuilding against an SDK that has the API turns it on with no code change.
 */
#pragma once

#include <zrnui/controls.h>
#include <zrnui/element.h>
#include <zrnui/host.h>
#include <zrnapp.h>

#include <cstdlib>

#ifndef ZWA_HAVE_SWIPE
/* the phases of Zelement_on_swipe, for an SDK without it (the shim then never calls back) */
typedef enum ZSwipePhase { Z_SWIPE_STARTED, Z_SWIPE_DELTA, Z_SWIPE_COMPLETED } ZSwipePhase;
#endif

namespace wa::gap {

/** G2: Holding (long press) on an element. Returns false when the SDK has none: the caller falls back to a tap. */
inline bool on_holding(const Zid element, const ZRoutedCallback callback, void* const user_data) {
#ifdef ZWA_HAVE_HOLDING
	Zelement_on_holding(element, callback, user_data);
	return true;
#else
	(void)element;
	(void)callback;
	(void)user_data;
	return false;
#endif
}

inline bool have_holding() {
#ifdef ZWA_HAVE_HOLDING
	return true;
#else
	return false;
#endif
}

/** G1: a callback after the theme or accent of the root changed (the shell's), so the app can re-apply its overrides. */
inline bool on_theme_changed(const Zid root, const ZRoutedCallback callback, void* const user_data) {
#ifdef ZWA_HAVE_THEME_CHANGED
	Zroot_on_theme_changed(root, callback, user_data);
	return true;
#else
	(void)root;
	(void)callback;
	(void)user_data;
	return false;
#endif
}

/** G3: the element of the old page that flies out in a continuum transition. Without it the turnstile runs. */
inline ZPageTransition continuum(const Zid frame, const Zid from) {
#ifdef ZWA_HAVE_CONTINUUM
	Zframe_set_continuum_element(frame, from);
	return Z_TRANSITION_CONTINUUM;
#else
	(void)frame;
	(void)from;
	return Z_TRANSITION_TURNSTILE;
#endif
}

/** G3: the element of the new page the continuum flies into (call in the page factory). */
inline void continuum_target(const Zid page, const Zid element) {
#ifdef ZWA_HAVE_CONTINUUM
	Zpage_set_continuum_element(page, element);
#else
	(void)page;
	(void)element;
#endif
}

/** G4: the slide-up transition of the viewers. Without it the app turns the frame transition off and animates the page itself. */
inline bool have_slide_up() {
#ifdef ZWA_HAVE_SLIDE_UP
	return true;
#else
	return false;
#endif
}

/** Navigates with a transition for this navigation only (G4); without the SDK call: set, navigate, restore. */
inline bool navigate_with(const Zid frame, const ZPageFactory factory, void* const parameter, const ZPageTransition transition) {
#ifdef ZWA_HAVE_NAVIGATE_WITH
	return Zframe_navigate_with_transition(frame, factory, parameter, transition);
#else
	Zframe_set_transition(frame, transition);
	const bool b_ok = Zframe_navigate(frame, factory, parameter);
	Zframe_set_transition(frame, Z_TRANSITION_TURNSTILE);
	return b_ok;
#endif
}

inline ZPageTransition slide_up() {
#ifdef ZWA_HAVE_SLIDE_UP
	return Z_TRANSITION_SLIDE_UP;
#else
	return Z_TRANSITION_NONE;
#endif
}

/** G6: render a row once into a texture. Without it the row is painted every frame it is in view.
 * On by default; ZWA_ROW_CACHE=0 turns it off. A smear seen across a cached row (QA 2026-11-20, 4/18 shots) was not the
 * cache's: the S9's GPU faults now and then in a job (panfrost, in every GL client), the cache only kept the bad pixels.
 * zrnui now paints a settle frame at rest that makes every cache again (root.h). */
inline void cache_bitmap(const Zid element) {
#ifdef ZWA_HAVE_CACHE_MODE
	static const bool b_on = [] {
		const char* const v = getenv("ZWA_ROW_CACHE");
		return !v || *v != '0';
	}();
	if (!b_on) return;
	Zelement_set_cache_mode(element, Z_CACHE_BITMAP);
#else
	(void)element;
#endif
}

/** G6 off again, for a row whose inside moves (a swipe): a cached row would show the old bitmap. */
inline void cache_none(const Zid element) {
#ifdef ZWA_HAVE_CACHE_MODE
	Zelement_set_cache_mode(element, Z_CACHE_NONE);
#else
	(void)element;
#endif
}

inline bool have_anchor() {
#ifdef ZWA_HAVE_ANCHOR
	return true;
#else
	return false;
#endif
}

/** G9: keep `element` fixed on screen across size changes above it. Returns false without it (the app anchors by hand). */
inline bool anchor(const Zid scroll_viewer, const Zid element) {
#ifdef ZWA_HAVE_ANCHOR
	Zscroll_viewer_set_anchor(scroll_viewer, element);
	return true;
#else
	(void)scroll_viewer;
	(void)element;
	return false;
#endif
}

/** A horizontal swipe on a row (reply). Without it the menu's "reply" is the way. */
using SwipeFn = void (*)(Zid, ZSwipePhase, float, void*);
inline bool on_swipe(const Zid element, SwipeFn callback) {
#ifdef ZWA_HAVE_SWIPE
	Zelement_on_swipe(element, callback, nullptr);
	return true;
#else
	(void)element;
	(void)callback;
	return false;
#endif
}

/** G5: the keyboard's height as it changes (zrnapp shrinks the root by it itself). */
inline bool on_input_pane(void (*callback)(float, void*), void* const user_data) {
#ifdef ZWA_HAVE_INPUT_PANE
	Zwindow_on_input_pane(callback, user_data);
	return true;
#else
	(void)callback;
	(void)user_data;
	return false;
#endif
}

inline bool have_theme_follow() {
#ifdef ZWA_HAVE_THEME_CHANGED
	return true;
#else
	return false;
#endif
}

}  // namespace wa::gap
