# zrn-whatsapp UI spec (zrnui v2)

Target: the WhatsApp client of Windows Phone 8.1, rebuilt on zrnui v2 (`zrn-sdk/zrnui`). This document is the design authority. A reviewer checks the implementation against section 9.

## 0. Ground rules

| Rule | Value |
|---|---|
| Units | zrnui logical px (WP8.1 effective px). On the S9 the shell scale gives a 360 x 740 grid. |
| Grid | WP 24/12 grid in zrnui px is **19 / 9.5**. Page side margin 19. Gap between blocks 9.5. Every margin in this spec is a multiple of 9.5 (or 4.75 inside bubbles). |
| Colours | Theme keys only (`Z_RES_*`, bound with `*_bind_*`). No literal colour in app code except L1-L3 (5.1). |
| Fonts | Selawik via theme families (Normal, SemiLight, SemiBold). Sizes via `Z_RES_TEXT_STYLE_*` keys or `Z_TEXT_STYLE_*`. |
| Case | Pivot headers and page titles lower case (`chats`), app title upper case (`WHATSAPP`). App bar labels lower case. Menu items lower case. Body text sentence case. |
| Transitions | Frame transition `Z_TRANSITION_TURNSTILE` everywhere unless stated. |
| Tilt | On for every tappable row/tile (`Zelement_set_tilt`). Off for bubbles while scrolling (see 4). |
| Animations off | `Zroot_animations_enabled` false: everything jumps, no layout differs. |

Font size keys used below (values from `theme_resources.c`):

| Key | px | Use |
|---|---|---|
| `Z_RES_TEXT_STYLE_SMALL_FONT_SIZE` | 10.66 | never for content; only badges |
| `Z_RES_TEXT_STYLE_MEDIUM_FONT_SIZE` | 16 | previews, timestamps, captions, bubble meta |
| `Z_RES_TEXT_STYLE_LARGE_FONT_SIZE` | 18.14 | pivot title, page title, bubble body, settings secondary |
| `Z_RES_CONTENT_CONTROL_FONT_SIZE` | 20.26 | text box, buttons, menu items, settings primary |
| `Z_RES_TEXT_STYLE_EXTRA_LARGE_FONT_SIZE` | 25.5 | list row primary line (chat name, contact name) |
| `Z_RES_TEXT_STYLE_EXTRA_EXTRA_LARGE_FONT_SIZE` | 34.14 | chat page header name, empty-state heading, pairing code |
| `Z_RES_PIVOT_HEADER_ITEM_FONT_SIZE` | 57 | pivot headers and page headers |

## 1. Screens and navigation

One `Zframe`. Back = system back / `Zframe_go_back`. Every page except the main pivot has a back stack entry.

| # | Screen | Kind | Title (small, `PIVOT_TITLE` style 18.14 SemiBold, upper case) | Header (57 SemiLight, lower case) | Cache mode | Back goes to |
|---|---|---|---|---|---|---|
| S1 | Main | `Zpivot` on a page | `WHATSAPP` | pivot items `chats`, `status`, `channels` | Required | exits app (shell) |
| S2 | Chat | page, custom header (2.2) | none | none (contact header instead) | Disabled (rebuilt per chat) | S1, keeps pivot index and scroll |
| S3 | New chat | page | `WHATSAPP` | `new chat` | Disabled | S1 |
| S4 | Chat info | `Zpivot` | chat name upper case | `info`, `media` (media only if engine lists media; else no pivot, plain page header `info`) | Disabled | S2 |
| S5 | Settings | page | `WHATSAPP` | `settings` | Disabled | S1 |
| S6 | Account | page | `WHATSAPP` | `account` | Disabled | S5 |
| S7 | Link | page, no back stack (root while unlinked) | `WHATSAPP` | `link device` | Disabled | exits app |
| S8 | Media viewer | page, black chrome, full bleed | none | none | Disabled | caller |
| S9 | Status viewer | page, full bleed | none | none | Disabled | S1 status pivot |
| S10 | New status | page | `WHATSAPP` | `new status` | Disabled | S1 |
| S11 | Search | page | `WHATSAPP` | `search` | Disabled | S1 |
| S12 | Starred / Archived | page each | `WHATSAPP` | `starred` / `archived` | Disabled | S1 |
| S13 | Follow channel | page | `WHATSAPP` | `follow channel` | Disabled | S1 channels |
| S14 | Stickers | `Zflyout` (full-width, bottom placement) over S2, not a page | — | — | — | closes flyout |

Navigation rules:
- Unlinked (`pair_state` not linked): S7 is the only page; `Zframe_navigate` S7 and clear the back stack. On link success navigate S1 and clear the stack.
- Tapping a chat row: navigate S2 (turnstile; see 4.1 for the continuum gap).
- Notification activation (`ztwhatsapp.focus`): S1 then S2 on the stack, so back lands on S1.
- Back on S2 with the keyboard open: first back closes keyboard only (WP behaviour).
- Back with app bar expanded or flyout open: closes it only.
- Starred and archived move off the pivot into the app bar menu (5 pivot items is too many on 360 px; WP WhatsApp had 3).

## 2. Controls per screen

### 2.1 S1 Main pivot

Pivot: `Zpivot_set_title("WHATSAPP")`, items margin from `Z_RES_PIVOT_ITEM_MARGIN` (19, 22.5, 19, 0). Unselected headers use the pivot default colour; do not override.

**chats** item: `Zlist_view`, selection None, item click on, tilt on (default). Item template (custom element added with `Zitems_control_add`):

| Part | Control | Size / position | Font / colour |
|---|---|---|---|
| Row | `Zgrid` 3 cols (58, *, auto), 2 rows | height 76 (58 + 2 x 9.5), no side margin (pivot gives 19) | — |
| Avatar | `Zborder` 58 x 58, square (corner 0), `Zimage` inside | col 0, rows 0-1, right margin 9.5 | placeholder background `Z_RES_PHONE_ACCENT_BRUSH`, monogram 1 letter `TEXT_STYLE_EXTRA_LARGE` SemiLight, `Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR`, centred. Group without picture: `Z_SYMBOL_PEOPLE` icon 28 px same colours |
| Name | `Ztext_block` | col 1 row 0, trimming CharacterEllipsis, no wrap | `TEXT_STYLE_EXTRA_LARGE` (25.5) SemiLight, `PHONE_FOREGROUND_BRUSH` |
| Time | `Ztext_block` | col 2 row 0, left margin 9.5, baseline aligned with name | `TEXT_STYLE_MEDIUM` (16); unread: `PHONE_ACCENT_BRUSH`, else `Z_RES_PHONE_BASE_MEDIUM_MID_COLOR` |
| Preview | `Ztext_block` | col 1 row 1, one line, ellipsis | 16, `BASE_MEDIUM_MID`; unread: `PHONE_FOREGROUND_BRUSH`. Prefix: tick glyph (2.3) for own last message, `sender: ` in groups, "typing..." in `PHONE_ACCENT_BRUSH` while presence composing |
| Unread count | `Zborder` min 22 x 22, padding 4.75,0, corner 0 | col 2 row 1, right aligned | background `PHONE_ACCENT_BRUSH`, text 16 SemiBold `TEXT_OVER_ACCENT` |
| Muted | `Zsymbol_icon` `Z_SYMBOL_MUTE` 16 px | col 2 row 1 left of count | `BASE_MEDIUM_MID` |
| Pinned | `Zsymbol_icon` `Z_SYMBOL_PIN` 16 px | col 2 row 1 | `BASE_MEDIUM_MID` |

Row press: `Zmenu_flyout` on long press (SDK gap G2) with `pin to top` / `unpin`, `mute` / `unmute`, `archive`, `mark as read`, `delete chat`.

**status** item: `Zlist_view`. Top row "my status" (avatar + `add status` in 25.5 + subtitle 16 "tap to add a status update"); then `Zlist_view_add_group_header("recent")` and `("viewed")`. Avatar of a person with unseen items gets a 3 px `PHONE_ACCENT_BRUSH` border (`Zborder` border thickness 3); seen: border `BASE_LOW`. Row = chats template minus count.

**channels** item: `Zlist_view`, same template; preview = channel description. Footer `Zhyperlink_button` `follow a channel` (CONTENT_CONTROL size, accent) navigating S13.

**App bar** (`Zpage_set_bottom_app_bar`, `Zcommand_bar`, closed display mode Compact). Changes with pivot index:

| Pivot | Primary buttons (round, `Zapp_bar_button_create`, label lower case) | Secondary (`...` menu items) |
|---|---|---|
| chats | `new` (`Z_SYMBOL_ADD`), `search` (`Z_SYMBOL_FIND`), `select` hidden (no multi-select in v1) | `starred messages`, `archived chats`, `settings`, `account` |
| status | `new` (`Z_SYMBOL_ADD`) | `settings` |
| channels | `follow` (`Z_SYMBOL_ADD`), `search` | `settings` |

Maximum 4 primary buttons (WP rule). Buttons disabled (not hidden) when the engine socket is down.

### 2.2 S2 Chat

Layout top to bottom: header (fixed) / connection banner (6) / message list (fills) / composer (fixed) / app bar.

| Part | Control | Size | Font / colour |
|---|---|---|---|
| Header | `Zgrid` (auto avatar, *), margin 19,9.5,19,9.5 | height 57 | — |
| Header avatar | 38 x 38 square image or accent monogram | right margin 9.5 | as 2.1, monogram 18.14 |
| Header name | `Ztext_block` upper case, ellipsis | — | `TEXT_STYLE_LARGE` 18.14 SemiBold, foreground |
| Header status | `Ztext_block` | under name | 16, `BASE_MEDIUM_MID`; "online" / "last seen today at 18:02" / "typing..." (accent) / group: member first names, ellipsis |
| Header tap | whole header is a `Zbutton` with transparent background and no border, tilt on | — | opens S4 |
| Message list | `Zlist_view` selection None, item click off, `Zitems_stack_panel` | margin 9.5,0,9.5,0 | see 3 |
| Composer | `Zgrid` (*, auto) margin 9.5,4.75,9.5,4.75 | min 47, grows to 4 lines then scrolls | — |
| Text box | `Ztext_box`, accepts return, wrap, placeholder `type a message` | col 0 | `CONTENT_CONTROL` 20.26, theme text box brushes (accent border on focus) |
| Emoji button | `Zbutton` 38 x 38, no border, `Z_SYMBOL_EMOJI2` | col 1, inside composer right | foreground `BASE_MEDIUM_MID`, pressed accent; opens S14 |

App bar on S2 (Compact; WP WhatsApp style):

| Primary | Icon | Enabled |
|---|---|---|
| `send` | `Z_SYMBOL_SEND` | only when the text box has non-space text |
| `attach` | `Z_SYMBOL_ATTACH` | flyout: `photo`, `camera` (disabled until engine supports), `sticker` |
| `voice` | `Z_SYMBOL_MICROPHONE` | hidden until engine sends voice; do not show a dead button |

Secondary: `view contact` / `group info` (S4), `search`, `mute notifications` / `unmute`, `clear chat`, `pin to start` (only if shell supports; else omit). Read-only channel: composer hidden, app bar secondary `unfollow` only.

Message context (long press on a bubble, gap G2; until then single tap): `Zmenu_flyout` items `reply`, `copy`, `forward` (if engine), `star` / `unstar`, `edit` (own text, under 15 min), `delete for me`, `delete for everyone` (own, engine allows), `save sticker` (sticker), `save` (image). Destructive items open a `Zcontent_dialog` (primary `delete`, secondary `cancel`). No "tap again" toasts.

Reply mode: a strip above the composer, `Zgrid` margin 9.5, left bar 4.75 wide `PHONE_ACCENT_BRUSH`, sender 16 SemiBold accent, one line quote 16 `BASE_MEDIUM_MID`, close `Zbutton` `Z_SYMBOL_CANCEL` 38 x 38. Edit mode: same strip, label `editing`.

### 2.3 Ticks and glyphs

Draw ticks as `Zpath_icon` (16 x 11), not text:

| State | Glyph | Colour |
|---|---|---|
| pending | clock path | `BASE_MEDIUM_MID` (in an accent bubble: `TEXT_OVER_ACCENT` at 0.6 opacity) |
| sent | single check | same as pending |
| delivered | double check | same as pending |
| read | double check | `Z_RES_SYSTEM_COLOR_CONTROL_ACCENT_COLOR` on incoming-coloured surfaces; inside an accent bubble use `TEXT_OVER_ACCENT` at 1.0 (dim states use 0.6) |
| failed | `!` in a 16 px circle | literal red L1 (5.1); bubble tap = retry flyout `retry` / `edit` / `delete` |

### 2.4 S3 New chat

`Ztext_box` placeholder `name or number`, margin 19,0,19,9.5. Below: `Zlist_view` grouped alphabetically (`Zlist_view_add_group_header` per letter, `Zsemantic_zoom` jump list A-Z with the zoomed-out view as accent letter tiles 46 x 46). Row: avatar 58, name 25.5, phone number 16 `BASE_MEDIUM_MID`. Typed number that matches no contact: first row `message +<digits>` with `Z_SYMBOL_SEND` avatar. App bar: none (back only). Empty: 6.

### 2.5 S4 Chat info

Big picture: `Zimage` full width 322 (360 - 2 x 19), square, tap opens S8. Then sections with group headers (`Z_TEXT_STYLE_GROUP_HEADER`, accent, lower case): `about and phone number`, `settings`, `participants` (groups), `encryption`.

| Control | Type |
|---|---|
| Mute, pin, archive | `Ztoggle_switch` with header text, full width, margin 0,0,0,9.5 |
| Phone number | `Zlist_view_item` row: number 20.26, "mobile" 16 subtle; tap: menu flyout `call` (disabled), `copy` |
| Participants | list rows 58 avatar; admins get `admin` 16 accent at right |
| Destructive | plain `Zbutton` full width (`exit group`, `block`, `clear chat`), foreground text; each confirms with `Zcontent_dialog` |

### 2.6 S5 Settings

Plain page, `Zscroll_viewer`, stack of `Ztoggle_switch` (header = setting name 20.26, on/off text default) margin 0,0,0,19:

| Setting | Control |
|---|---|
| notifications | toggle |
| show message preview | toggle |
| enter key sends | toggle |
| download stickers automatically | toggle |
| use WhatsApp green instead of the accent | toggle (default off: follow system accent, live) |
| account | `Zlist_view_item` row "account" 25.5 + "linked device, unlink" 16 subtle, navigates S6 |
| about | text 16 subtle at the bottom: version, unofficial-client notice |

S6 Account: own number 25.5, linked state 16 subtle, one `Zbutton` `unlink this phone` (full width) then `Zcontent_dialog` title `unlink?`, content "Messages stored on this phone are deleted.", primary `unlink`, secondary `cancel`.

### 2.7 S7 Link

| State | Content |
|---|---|
| unpaired | notice paragraph 18.14 wrap (ToS risk text, unchanged meaning); `Zbutton` `show QR code` full width; `Ztext_box` header `phone number with country code`, input scope number; `Zbutton` `get a code` full width. Error text 16 in L1 red above buttons |
| qr | instruction 18.14 wrap; QR `Zimage` integer module size, centred, max 304 px, white quiet zone 4 modules on `PHONE_BACKGROUND` (QR itself always black on white in both themes); `Zprogress_bar` indeterminate under it while waiting for refresh; `Zbutton` `cancel` |
| phone_code | instruction 18.14; code in `TEXT_STYLE_EXTRA_EXTRA_LARGE` 34.14 SemiBold, letter-spaced as `XXXX-XXXX`, selectable (`Ztext_box` read-only, no border) ; `Zbutton` `cancel` |
| pairing | `Zprogress_ring` 46 px centred + "finishing the link..." 18.14 |
| no engine | `Zprogress_bar` indeterminate at the very top (WP system tray style) + "waiting for the WhatsApp service..." 18.14 subtle |

### 2.8 S8 Media viewer

Full bleed `Zscroll_viewer` with zoom (pinch, double-tap 1x/2.5x) holding a `Zimage`. Background literal black in both themes (photo viewer convention, L2). Chrome (top caption: sender + time 16 white, bottom app bar `save`, `share` disabled, `forward`) toggles on tap, fade 167 ms. Video: `Z_SYMBOL_PLAY` 57 px circle centred, opens external player.

### 2.9 S9 Status viewer

`Zflip_view` of items, background literal black. Top: segmented `Zprogress_bar`s (one per item, 2 px tall, gap 4.75, margin 9.5) white on white-at-0.3; name 18.14 SemiBold + time 16 white at 0.7. Text status: full-page `Zborder` filled with the status's background colour from the engine (else accent), text 34.14 SemiLight centred wrap, `TEXT_OVER_ACCENT`. Auto-advance 5 s (images), tap right half next, left half previous, back exits.

### 2.10 S10-S13

S10 New status: `Ztext_box` multiline, placeholder `type a status`, max length 700 bytes counter `n/700` 16 subtle right aligned; app bar `post` (`Z_SYMBOL_SEND`). S11 Search: `Ztext_box` with `Z_SYMBOL_FIND` placeholder `search chats and messages`; results list with group headers `chats` and `messages`; message rows show chat name 20.26 + snippet 16 with the match in accent. S12: chats template / message template. S13: `Ztext_box` header `invite link or code`, app bar `follow` (`Z_SYMBOL_ACCEPT`).

### 2.11 S14 Stickers flyout

`Zflyout` placement Bottom, full width, height 285 (3 rows). `Zgrid_view` of 85 x 85 cells, 4 per row, item margin 4.75, tilt on. Tap sends and closes. Empty: "no saved stickers yet" 16 subtle centred.

## 3. Chat bubbles

### 3.1 Geometry (WP WhatsApp style: flat rectangle, no rounded corners, triangular tail)

| Item | Value |
|---|---|
| Shape | `Zborder`, corner radius 0, no border |
| Max width | 75 % of list width (= 255 px on S9), min width 76 (fits time + ticks) |
| Padding | 9.5 left/right, 4.75 top, 4.75 bottom |
| Tail | `Zpath_icon` right triangle 12 x 9.5, same colour as the bubble. Incoming: above the bubble at its left edge, pointing up (WP messaging top-left tail). Outgoing: below the bubble at its right edge, pointing down. Path data incoming `M0,9.5 L0,0 L12,9.5 Z`, outgoing `M0,0 L12,0 L12,9.5 Z` |
| Tail grouping | Consecutive messages from the same sender within 60 s: only the first incoming / last outgoing gets the tail; gap between grouped bubbles 4.75, between groups 14.25 (tail space included) |
| Alignment | Outgoing right, margin left 47.5; incoming left, margin right 47.5 |
| Group sender | first bubble of a group in a group chat: sender name 16 SemiBold, colour = `PHONE_ACCENT_BRUSH` on incoming |

### 3.2 Colours

| Surface | Dark | Light | Keys |
|---|---|---|---|
| Outgoing bubble | accent | accent | `Z_RES_PHONE_ACCENT_BRUSH` |
| Outgoing text, time, ticks | white | white | `Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR` (time/ticks at opacity 0.7 except read ticks 1.0) |
| Incoming bubble | `#1F1F1F` | `#DDDDDD` | `Z_RES_PHONE_CHROME_COLOR` |
| Incoming text | foreground | foreground | `Z_RES_PHONE_FOREGROUND_BRUSH` |
| Incoming time | | | `Z_RES_PHONE_BASE_MEDIUM_MID_COLOR` |
| Page background | black | white | `Z_RES_PHONE_BACKGROUND_BRUSH` (no wallpaper) |
| System line / date chip | | | text `BASE_MEDIUM_MID`, no fill |

Accent: the system accent by default (live). The settings toggle replaces the accent of this root with WhatsApp green L3 via `Zroot_set_accent_color`; that is the only place green is used.

### 3.3 Content parts (top to bottom inside a bubble)

| Part | Spec |
|---|---|
| Sender name | 3.1 |
| Reply quote | `Zgrid`: 4.75-wide bar (incoming: accent; outgoing: `TEXT_OVER_ACCENT`) + column with sender 16 SemiBold and one-line quote 16 (opacity 0.8), ellipsis. Background `Z_RES_PHONE_BASE_LOW_COLOR` overlay. Margin bottom 4.75. Tap scrolls to the quoted message (`Zlist_view_scroll_into_view_with_alignment` centre) and flashes it (opacity 0.5 then 1, 2 x 167 ms) |
| Image | width = bubble max width minus padding (236), height = width x aspect clamped 95..304; blurred thumb from `thumb_b64` immediately, full image swaps in place with a 167 ms cross-fade, same size (no jump). Caption below as body text. Image-only: time overlaid bottom-right on a `PHONE_SEMITRANSPARENT` 4.75-padded strip |
| Body text | `TEXT_STYLE_LARGE` 18.14 Normal, wrap, selectable via flyout `copy` only. Links in accent underline (incoming) / `TEXT_OVER_ACCENT` underline (outgoing), tap opens browser. A message of 1-3 emoji only: 34.14, no bubble fill change |
| Meta line | right aligned, same line as the last text line when it fits, else its own line: `edited` 16 (no italic face), star `Z_SYMBOL_FAVORITE` 13, time `HH:MM` 16, ticks (outgoing) |
| Reactions | below the bubble, overlapping its bottom by 9.5, at the outer edge: `Zborder` `PHONE_CHROME_COLOR` with 2 px `PHONE_BACKGROUND` border, padding 4.75, emoji 16 + count 16. Adds 14.25 below the bubble |
| Sticker | no bubble, no tail; 142 x 142 image; meta line under it in a `PHONE_SEMITRANSPARENT` chip, aligned to the sender side |
| Voice / doc / other kinds | one-line row: `Zsymbol_icon` (`Z_SYMBOL_MICROPHONE`, `Z_SYMBOL_PAGE`) 20 + label 18.14 ("voice message 0:12", file name) |
| Deleted | text "this message was deleted" 18.14 at opacity 0.6, no ticks |

### 3.4 List-level items

| Item | Spec |
|---|---|
| Date separator | centred, `TEXT_STYLE_MEDIUM` 16 SemiBold lower case ("today", "yesterday", "monday", "19 november 2026"), `BASE_MEDIUM_MID`, margin 19 top / 9.5 bottom |
| System message | centred, 16, `BASE_MEDIUM_MID`, wrap, max width 85 % |
| Unread divider | full-width band, height 28.5, background `BASE_LOW`, text "n unread messages" 16 SemiBold accent centred. Inserted once on opening; stays until the chat is left. Initial scroll puts it at the top third |
| Typing indicator | an incoming bubble (with tail) containing three 6.5 px squares, `BASE_MEDIUM_MID`, opacity pulse 0.3-1.0, 1200 ms cycle, 200 ms stagger, `Z_EASE_LINEAR`. In groups: sender name above it. Removed with fade 167 ms; the real message replaces it in the same slot with no jump |
| Load older | when scrolled within 1 screen of the top, request older; show a 2 px `Zprogress_bar` indeterminate at the list top; keep scroll anchored on the first visible message after insert |
| Jump to bottom | when more than 1 screen from bottom: round 38 px `Zbutton` with a `Zpath_icon` down chevron (no `Z_SYMBOL_DOWN`), bottom-right, margin 19, background `PHONE_CHROME_COLOR`; new unseen messages show an accent count badge on it |

## 4. Animations

All via `Zframe` transitions or `Zelement_animate`. Durations in ms.

| # | Event | Spec |
|---|---|---|
| 4.1 | Page forward/back (all pages) | `Z_TRANSITION_TURNSTILE` (706 ms total, built in). Chat row to S2: WP used continuum (row name flies to header) - gap G3; until then turnstile |
| 4.2 | Media viewer / status viewer open | slide up + fade: content `TRANSLATE_Y` 150 to 0 and opacity 0 to 1, 350, `Z_EASE_EMPHASIZED`; close reverse 250 `Z_EASE_IN`. Needs frame transition NONE for these navigations (`Zframe_set_transition` before navigate, restore after) - see G4 |
| 4.3 | List entrance (pivot item first shown, search results) | rows in view only: each row `ROTATION_Y` -60 to 0 (projection centre x 0) and opacity 0 to 1, 300 `Z_EASE_EMPHASIZED`, delay 30 x index, max 10 rows. Rows outside the view appear without animation |
| 4.4 | Tilt | built-in `Zelement_set_tilt` on rows, tiles, header button, sticker cells. Not on bubbles (they open a flyout; tilt on a long list of bubbles reads as wobble) |
| 4.5 | App bar expand / menu | built-in `Zcommand_bar` behaviour; do not override |
| 4.6 | Incoming message insert (at bottom, list at end) | new row height animates from 0 is not possible without relayout cost: instead append, scroll to end with a 250 `Z_EASE_OUT` scroll, row `TRANSLATE_Y` 28.5 to 0 + opacity 0 to 1, 250 `Z_EASE_OUT` |
| 4.7 | Incoming when not at end | no scroll; jump button badge increments with `SCALE` 1.3 to 1, 167 `Z_EASE_OUT` |
| 4.8 | Send | text box clears at once; outgoing bubble appended with `TRANSLATE_Y` 47.5 to 0, opacity 0.0 to 1, 200 `Z_EASE_EMPHASIZED`; clock tick shown; tick changes cross-fade 120 |
| 4.9 | Keyboard show / hide | list bottom follows the composer; keep the last visible message's bottom edge fixed (anchor) so the content scrolls with the keyboard, no jump. Needs the keyboard inset (gap G5) |
| 4.10 | Chat list reorder (new message moves chat to top) | moved row: opacity 0.4 to 1, 200 `Z_EASE_OUT` at its new place; others jump. No sliding of every row |
| 4.11 | Banner (6) show/hide | `TRANSLATE_Y` -28.5 to 0 + opacity, 200 `Z_EASE_OUT`; content below does not move (banner overlays the list top) |
| 4.12 | Flyouts, dialogs | built in |

Every animation runs on render properties only (opacity, translate, rotation, scale): no animated width/height/margin.

## 5. Light / dark and accent

| Use | Key |
|---|---|
| Page background | `Z_RES_PHONE_BACKGROUND_BRUSH` |
| Primary text | `Z_RES_PHONE_FOREGROUND_BRUSH` |
| Secondary text, time, meta | `Z_RES_PHONE_BASE_MEDIUM_MID_COLOR` |
| Faint fills (unread divider, quote overlay) | `Z_RES_PHONE_BASE_LOW_COLOR` |
| Incoming bubble, chips, jump button | `Z_RES_PHONE_CHROME_COLOR` |
| Outgoing bubble, unread count, accent text, avatar placeholder, focus border | `Z_RES_PHONE_ACCENT_BRUSH` / `Z_RES_SYSTEM_COLOR_CONTROL_ACCENT_COLOR` |
| Text on accent | `Z_RES_PHONE_TEXT_OVER_ACCENT_COLOR` |
| Disabled | `Z_RES_PHONE_DISABLED_BRUSH` |
| Image overlays | `PhoneSemitransparent` equivalent: `Z_RES_CONTENT_DIALOG_DIMMING_COLOR` |
| Text box | theme `Z_RES_TEXT_BOX_*` (do not restyle) |

Rules:
- Everything is bound (`Ztext_block_bind_foreground`, `Zborder_bind_background`, `Zcontrol_bind_*`), so a theme or accent change repaints with no rebuild.
- The theme and accent follow the shell live (gap G1). On change: no page rebuild, no scroll reset, no flash.
- Pictures (avatars, media) are not tinted.

### 5.1 Literal colours allowed (only these)

| Id | Colour | Where |
|---|---|---|
| L1 | `#FFE51400` (WP Red accent) | failed-send glyph and link errors |
| L2 | `#FF000000` / white text | media and status viewers |
| L3 | `#FF25D366` | optional WhatsApp-green accent override |

## 6. States

| State | Where | Spec |
|---|---|---|
| Empty chats | S1 chats | `Z_TEXT_STYLE_LIST_VIEW_EMPTY_STATIC`-like: 25.5 SemiLight `BASE_MEDIUM_MID`, "no chats yet", second line 16 "tap new to start one", margin 0,19 |
| Empty status / channels / starred / archived / search / contacts | each list | same style: "no status updates", "you follow no channels", "no starred messages", "no archived chats", "nothing found", "no contacts yet - they arrive from your phone" |
| Loading list | any list before first reply | `Zprogress_bar` indeterminate 2 px at the top of the page (WP system-tray progress), no spinners in rows, no skeletons |
| Loading chat | S2 | header renders at once from the chat list data; list empty with the top progress bar; no "loading" text |
| Connecting | all pages | banner below the header / pivot title: height 28.5, `PHONE_CHROME_COLOR`, text 16 "connecting..." + indeterminate bar in it. Shown only after 1.5 s of not-connected (avoid flicker) |
| Offline | all pages | same banner, text "waiting for network", no bar |
| Engine down | all pages | banner "WhatsApp service not running"; app bar buttons disabled; sending queues nothing (send disabled) |
| Send failed | bubble | L1 glyph replaces ticks; tap gives retry flyout |
| Request error | anywhere | no toasts (not a WP pattern): inline text under the control (16, L1), or a `Zcontent_dialog` for failed destructive actions |
| Unlinked | app | S7 only (2.7) |
| Logged out remotely | app | S7 with error line "this device was unlinked from your phone" |

## 7. Quality bar

| Area | Requirement |
|---|---|
| Frame time | 60 fps on the 4 efficiency cores alone (big cores offline): p95 frame under 16.7 ms, no frame over 33 ms while scrolling a 500-message chat, flinging the chat list, pivot swipe, turnstile. GL renderer only (render transforms need it). |
| Rows painted once | a row's text is shaped and its layout measured once per content change; scrolling must not re-shape text or re-decode images. Bubble template built once per message; `bind` only on data change. See G6 for a render cache. |
| Images | thumbs decoded off the UI thread; decoded size = display size x scale (no 4000 px bitmaps); LRU cache bounded (64 MB). |
| No layout jumps | heights known before paint: image size from engine width/height; sticker fixed 142; full image swaps into the same box; typing indicator, unread divider and load-older keep the anchor message fixed on screen. Opening a chat lands at the unread divider or the bottom in the first painted frame (no visible scroll). |
| Text clipping | single-line fields use CharacterEllipsis, never hard clip; no descender clipping (line height >= 1.33 x size); bubbles wrap on word, break long words/URLs at any character; header name ellipsis before status text overlaps; no text under the app bar or keyboard. |
| Emoji | colour emoji in every text block and the text box, baseline-aligned, same advance as the text size (gap G7). Until G7: monochrome fallback glyph, never tofu boxes. Emoji-only messages at 34.14. |
| Input latency | key to glyph under 50 ms; send tap to bubble on screen under 100 ms (optimistic). |
| Memory | under 120 MB RSS with 500 chats and a 2,000-message chat loaded. |
| Theme | dark/light/accent switch repaints within one frame, no rebuild, scroll kept. |

### Acceptance checklist (review uses these numbers)

1. S1 is a Pivot titled `WHATSAPP` with exactly `chats`, `status`, `channels`, headers lower case at 57 px.
2. Starred, archived, settings, account reachable from the app bar `...` menu; app bar has at most 4 round buttons with lower-case labels.
3. Every page has the WP title + lower-case header, except S2, S8, S9.
4. All side margins are 19; all gaps are multiples of 4.75; checked on 5 screens with a pixel ruler.
5. Chat row: 58 px square avatar, name 25.5 SemiLight, preview 16, time and count in accent when unread, ellipsis on long names.
6. Avatars, badges and reaction chips are square: no circles in the app except app bar buttons and the jump button.
7. S2 header shows avatar, name upper case 18.14 SemiBold, presence line; tapping it opens S4.
8. Bubbles: square corners, tails per 3.1, grouping per 3.1, max width 75 %.
9. Outgoing bubbles follow the system accent live; incoming use `PHONE_CHROME_COLOR`; both themes legible (contrast >= 4.5:1 for body text on both bubble colours with all 20 accents; for low-contrast accents (Yellow, Lime, Amber) text over accent switches to black via `TEXT_OVER_ACCENT`).
10. Ticks are drawn glyphs with the 5 states of 2.3; no "sent"/"read" words.
11. Reply quote, reactions, image, sticker, deleted, system line, date separator, unread divider, typing indicator all render as in 3.3/3.4.
12. Send button disabled with empty text; send clears the box and shows the bubble with the clock in under 100 ms.
13. Long press (or interim tap) on a bubble opens a Metro `Zmenu_flyout` with the 3.x items; destructive actions confirm with `Zcontent_dialog`; no "tap again" toasts.
14. Turnstile on every page change; list entrance per 4.3; tilt on every row; no tilt on bubbles.
15. No animated layout properties (code grep: no animation of width/height/margin).
16. Keyboard open keeps the last message visible and fixed (4.9).
17. New message at end scrolls smoothly; not at end, the jump button badge counts; no scroll stolen while reading history.
18. Loading older keeps the reading position to the pixel.
19. Opening a chat paints at the unread divider or the bottom on the first frame.
20. Connecting/offline banner appears after 1.5 s, overlays, does not shift content.
21. Every empty state of section 6 present with the given wording style.
22. Unlinked shows only S7; QR has integer module size and is sharp; phone code readable at 34.14 and selectable.
23. Theme and accent switch in the shell reach the open app within one frame, no rebuild, scroll kept.
24. Media viewer: black, pinch/double-tap zoom, chrome toggles on tap, slide-up open.
25. Frame time: p95 < 16.7 ms and no frame > 33 ms on the 4 efficiency cores, measured with the zrnui frame log during: chat list fling, 500-message chat fling, pivot swipe, page turnstile.
26. No text clipping or tofu on the test chats (long names, long URLs, RTL text, emoji-only, 4-line composer).
27. No colour literal in app code except L1-L3 (code grep).
28. Settings use `Ztoggle_switch`; unlink confirms with a dialog.
29. Back behaviour: closes keyboard, then flyout/app bar, then page; S1 back exits; S7 back exits.
30. Memory under 120 MB in the section 7 scenario.

## 8. SDK gaps (smallest addition that serves)

| Id | Need | Missing in zrnui v2 | Smallest addition |
|---|---|---|---|
| G1 | follow shell theme and accent live | zrnapp has no theme/accent source; app must poll/guess | zrnapp: read the shell's theme + accent at start and on change (the shell's existing channel), call `Zroot_set_theme` / `Zroot_set_accent_color`; plus `Zroot_on_theme_changed(root, cb, ud)` for apps with custom resources |
| G2 | long press (Holding) on rows and bubbles | no Holding / RightTapped event | `Zelement_on_holding(element, ZRoutedCallback, ud)` raised after 500 ms press without move > 9.5 px, cancelling tilt and click; plus `Zelement_set_context_flyout(element, flyout)` that shows it via `Zflyout_base_show_at` |
| G3 | continuum transition (chat row to chat header) | only NONE / TURNSTILE | `Z_TRANSITION_CONTINUUM` + `Zframe_set_continuum_element(frame, from_element)`: the element flies out (translate 0 to 100 x, -50 y, 150 ms ease-in) while the rest turnstiles; new page's marked element flies in |
| G4 | slide-up page transition (viewers, S10) | none | `Z_TRANSITION_SLIDE_UP` (WP SlideInTransition: translate Y 150 to 0 + fade, 350 ms emphasized) and per-navigation choice `Zframe_navigate_with_transition(frame, factory, param, transition)` |
| G5 | keyboard inset | no input-pane occluded rect / event | zrnapp: `Zwindow_on_input_pane(cb)` giving the occluded height from the text-input protocol, and the root shrinks its arrange height by it (WP InputPane) |
| G6 | rows painted once | "only what is in view is painted" but every frame re-rasterises visible rows | `Zelement_set_cache_mode(element, Z_CACHE_BITMAP)` (Silverlight CacheMode=BitmapCache): render the subtree to a GL texture, reuse until the subtree is invalidated |
| G7 | colour emoji | font.c has no colour glyph support (no CBDT/COLR, no fallback to an emoji face) | font fallback chain with Noto Color Emoji via FreeType `FT_LOAD_COLOR` (CBDT bitmaps), scaled to the text size, drawn as RGBA in the glyph atlas |
| G8 | large lists | items not virtualized (every container exists) | acceptable to 2,000 messages if G6 lands; otherwise `Zlist_view_set_item_source(count, factory, recycle)` with container recycling |
| G9 | scroll anchoring | not documented | `Zscroll_viewer_set_anchor(sv, element)`: keep that element's on-screen y fixed across content size changes above it (load older, typing indicator, image swap, keyboard) |

## 9. Critique of the current UI (ui/main.cpp, v1 libzrnui)

Screen locked during review (only the lock screen captured), so this is from the code.

Keep:
- Engine protocol use: paged history (`before_ts`/`before_id`), presence subscribe, mark-read on foreground, focus file for notifications.
- Picture loading one at a time (`pictures_next`) and thumb-first images.
- Sticker without bubble; failed message taps back into the draft.
- QR drawn with integer module size and sharp sampling.
- ToS risk notice before linking.

Wrong:
1. Five pivot tabs (`chats/status/starred/archived/channels`) on 360 px; WP used 3. Starred and archived belong in the menu.
2. Page titles in title case ("Link device", "New chat", "Settings"); Metro headers are lower case, app title upper case.
3. Ticks as words ("sent", "delivered", "read", "not sent, tap to edit") and star as `* `; must be glyphs.
4. Bubble is a generic `n.bubble()` with radius-10 images: no Metro tail, rounded look, no grouping, no accent outgoing colour control.
5. Reply quote is a plain caption with no bar or tap-to-jump; reactions and the typing indicator are not shown in the chat at all (reactions dropped in `msg_from`).
6. `guess_height` estimates row heights from byte counts: causes visible jumps when real layout differs (emoji, wrapping, images).
7. Day separator and system text use the same caption style; no unread divider.
8. Destructive actions use "tap again" toasts (unlink, delete); WP uses a confirm dialog. Toasts are not a WP pattern.
9. Chat footer puts stickers / send / info as centred row buttons between flex spacers; WP puts send/attach in the app bar and the emoji key inside the composer. "info" with a `more` icon is misleading.
10. Accent: hard-coded WhatsApp green by default; system-accent is opt-in and "applies the next time the app opens" - must be default and live.
11. Settings strings long and sentence-y in toggles ("Show message text in notifications"); shorten, lower-case Metro style.
12. Link page: phone field and buttons stacked with mixed variants (Primary/Tonal): Metro has one button style.
13. Status and channel pages are separate pushes with ad-hoc columns; no empty states; no loading indicator other than `app->busy`.
14. Avatars: monogram fallback fine, but rendering style (circle vs square) is v1's default; WP uses square.
