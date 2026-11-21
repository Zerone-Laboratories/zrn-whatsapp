# Windows Phone WhatsApp: reference measurements

Measured on phone screenshots of the WP 8.1 WhatsApp app (main list and group chat,
Nokia Lumia, 480 x 853 effective pixels). The phone screen was cropped (457 px wide in the
source picture) and scaled to the ztOS phone's 360-point width: 1 epx = 0.75 pt.

All y values are from the top of the phone screen. The WP status bar is 24 pt; the ztOS
status bar is 28 pt, so ztOS places everything 4 pt lower to keep the same distance from
its status bar (`wp::` constants in `ui/wa.h` are from the window top, under the status bar).

## Main page (pivot)

| Item | Reference | Notes |
|---|---|---|
| Status bar height | 24 | ztOS: 28 |
| Title "WHATSAPP" | caps top 36, cap height 11, ink x 16-100 | Segoe UI Bold 15.5, no extra spacing |
| Unread badge | bubble x 112-124, y 35-46; count "2" x 128-134 | accent; bubble 13 x 9 + tail; count Semibold 12.5 |
| Pivot headers | "h" top 61, baseline 90, x 16; "chats" ink 81 wide | Segoe UI Semilight 38.5, spacing -25 |
| Unselected header colour | about 0.45-0.5 white | zrnui uses PhoneDisabled 0.4 |
| Left margin | 15-16 | title, headers, avatars |
| Right margin | 19 | times and counts end at x 341 |
| Row pitch | 64 | |
| Avatar | 49 x 49, first top 107 (17 under the headers' baseline) | |
| Name | x 72.5, baseline = avatar top + 23.5, h-height 19.5 | Semilight 26, white |
| Time | baseline = name baseline, digits 11.5 tall | Regular 16, white |
| Preview | baseline = avatar top + 45.5, cap 10.5 | Regular 15; read: about 0.55-0.6 white (PhoneSubtle); unread: accent |
| Unread count | accent text, baseline = avatar top + 44.5 | Regular 16, no box |
| App bar | 49 tall chrome (#1F1F1F) from y 589 | ztOS: 1.5 border + 47.5 + 1.5 |
| App bar buttons | 33 circle, 2 ring, top 8 under the chrome's top, centres 59 apart, centred | order: search, add, (new group) |
| Glyphs | about 16 pt ink, Segoe UI Symbol (Find E11A, Add E109) | centred on ink |
| "..." | dots 3.5, 7.5 apart, middle 27 from right edge, tops level with circle tops | top corner, not centred |

## Chat

| Item | Reference | Notes |
|---|---|---|
| Title (name) | caps top 28 (4 under status bar), ink x 17 | Bold 15.5 capitals |
| Subtitle (status / members) | caps top 51, x 18, ink 194 wide for "Alice, Francis, Jack, Ned, Peter" | Regular 15, subtle |
| Picture in header | none | |
| Incoming bubble | x 18-290 (272 wide max), colour = accent (0,171,171 on teal) | |
| Outgoing bubble | x 70-340, colour = accent x 0.667 (0,114,114) | black 1/3 over the accent |
| Incoming tail | above the top-left: vertical side 9 from bubble left, 10 wide, 7 tall | |
| Outgoing tail | below the bottom-right: vertical side 9 from bubble right, 10 wide, 7 tall | |
| Gaps | 15 bubble to bubble; 5 between tails | |
| Sender name | baseline = bubble top + 16.5, x +9 | Regular 13, white 0.6 |
| Body text | first baseline +44.5 (with sender), line pitch 20, x +10 | Regular 16 (ink width), white |
| Time | baseline 9.5 above bubble bottom, right edge 10 inside | Regular 12.5, white 0.5; on the last text line when it fits |
| Voice note | ring 36 (2.5 ring) at x +14, top +31; line 2 pt x 76-204 at ring centre, white 0.3; time under line end; mic 7 wide before picture (green unheard, blue heard); picture 67 x 68 at right +7 inside, length over its corner | outgoing: picture on the left |
| Composer | mic at x about 32 (centre), box bubble x 68-341, 34 tall, tail under its right end | box 0.75-0.8 white unfocused |
| Recording | white mic left, "SLIDE TO CANCEL" centred (caps, subtle), time "0:03" about 30 pt, red mic right | |
| App bar | send (E122), attach (E16C), emoji (E11D), "..." top right | |
| Attach panel | "ATTACH MEDIA" caps, 3 x 2 accent tiles (albums, photo, video, audio, location, contact) | not implemented here |
| Media chooser | pivot "videos / pictures", month headers in accent, 4-wide thumbnails with play ring | not implemented here |
