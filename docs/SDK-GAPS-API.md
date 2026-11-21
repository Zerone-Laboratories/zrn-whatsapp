# SDK gaps: the final API (zrnui / zrnapp v2)

What landed for UI-SPEC.md section 8. Where it differs from the spec table, the difference is marked **changed**.

## G1 theme and accent, live
- zrnapp follows the shell by default: reads `theme` and `accent` from `~/.config/ztmetro/config` (or `$ZRNAPP_SHELL_CONFIG`) at window start, then re-reads on change (inotify on the directory, no polling) and calls `Zroot_set_theme` / `Zroot_set_accent_color`.
- `void Zroot_on_theme_changed(Zid root, ZRoutedCallback callback, void* user_data);` (sender = root; not called when the value is unchanged)
- **changed (addition):** `ZWindowOptions.b_keep_theme = true` opts out (app sets its own theme).

## G2 Holding and context flyout
- `void Zelement_on_holding(Zid element, ZRoutedCallback callback, void* user_data);` (same as `Zelement_on(element, Z_EVENT_HOLDING, ...)`)
- `void Zelement_set_context_flyout(Zid element, Zid flyout);` / `Zid Zelement_get_context_flyout(Zid element);`
- 500 ms, 9.5 px slop; the innermost element with a handler or flyout gets it; tilt, click and the release are cancelled; the flyout shows via `Zflyout_base_show_at` after the handler. The flyout is not owned.

## G3 continuum
- `Z_TRANSITION_CONTINUUM`
- `void Zframe_set_continuum_element(Zid frame, Zid from_element);` (taken by the next navigation)
- **changed (addition):** the new page marks its element with `void Zpage_set_continuum_element(Zid page, Zid element);` (call it in the page factory). It drops in: translate y -50 to 0, 250 ms ease-out, fading in. Back from a continuum page is a turnstile.

## G4 slide up, per-navigation transition
- `Z_TRANSITION_SLIDE_UP`
- `bool Zframe_navigate_with_transition(Zid frame, ZPageFactory factory, void* parameter, ZPageTransition transition);`
- Going back from a page runs the transition that opened it, backwards (slide down 250 ms ease-in + fade; the page below fades in 150 ms).

## G5 keyboard inset
- **changed:** `typedef void (*ZInputPaneCallback)(float occluded_height, void* user_data);`
  `void Zwindow_on_input_pane(ZInputPaneCallback callback, void* user_data);` (user_data added)
- zrnui: `void Zroot_set_input_pane_height(Zid root, float height);` / `float Zroot_get_input_pane_height(Zid root);` — the root arranges its tree in `height - inset`; popups keep the whole window.
- zrnapp does it all: when a TextBox / PasswordBox / AutoSuggestBox has focus it asks ztMETRO for its keyboard (`osk show <bottom>` on `$XDG_RUNTIME_DIR/ztmetro.ctl`), sets the inset, calls the callback; on focus loss `osk hide`, inset 0, callback(0).
- The height comes from the shell: ztMETRO writes `$XDG_RUNTIME_DIR/ztmetro-osk` (tmp + rename) whenever its keyboard shows, hides or changes height (orientation, terminal row), one line `<height in points> <visible 0|1>`, `0 0` while hidden. zrnapp follows it with inotify on the runtime directory and applies a new height at once (inset + callback) while a text box has focus. Without the file (older shell) it falls back to the config estimate `3 + 4 * key_height` (default 56 → 227). API unchanged.

## G6 rows painted once
- `typedef enum ZCacheMode { Z_CACHE_NONE = 0, Z_CACHE_BITMAP } ZCacheMode;`
- `void Zelement_set_cache_mode(Zid element, ZCacheMode mode);` / `ZCacheMode Zelement_get_cache_mode(Zid element);`
- GLES2 only (software ignores it). Repainted when something inside changes (property, layout, press, hover, focus), on theme/accent/scale change, or on resize. The element's own opacity and render transform apply to the texture. Content outside the element's bounds is cut. Max 2048 px a side.

## G7 colour emoji
- No API. Text drawn with Noto Color Emoji (CBDT) automatically, scaled to the text size, as RGBA. No shaping: ZWJ sequences draw as their separate pictographs; skin tones, VS15/16, keycap and tags take no room and are not drawn.

## G8 large lists
- Not added. Use G6 (`Z_CACHE_BITMAP` on each row container).

## G9 scroll anchoring
- `void Zscroll_viewer_set_anchor(Zid scroll_viewer, Zid element);` / `Zid Zscroll_viewer_get_anchor(Zid scroll_viewer);`
- Content above the anchor changing size moves the offset by the same amount. When the viewport height changes (keyboard), the anchor keeps its distance from the bottom edge. User or program scrolls move it as usual.
