# MacDeck protocol, version 1

The link between the Mac host and the deck running on the tablet.

## Transport

- The deck listens on **TCP `127.0.0.1:8765`** on the tablet. It accepts one client at a time; a new connection replaces the old one.
- Over USB, the Mac reaches it with `adb forward tcp:18765 tcp:8765` and connects to `127.0.0.1:18765`. The connection is bidirectional, so `adb reverse` is not needed (and isn't supported by this device's adbd).
- A future Wi-Fi transport will reuse the same messages, but it must add pairing and authentication first, because it leaves loopback.

## Framing

- UTF-8 text, one message per line, terminated by `\n`. Fields are separated by `|`. A field may not contain `|` or a newline; the host strips them.
- Maximum line length is **512 bytes**. Longer lines are discarded by the deck.
- Unknown message types are ignored, so either side can add messages without breaking the other.

## Deck → Mac

| Message | Meaning |
|---|---|
| `HELLO\|macdeck/1\|<build>\|<width>x<height>` | Sent immediately after the connection is accepted |
| `PONG\|<token>` | Reply to `PING` |
| `PRESS\|<button-id>` | A button was tapped (touch released inside the same button it started on) |
| `MACSET\|<key>\|<value>` | Change a Mac setting from the Mac page: `volume\|0-100` (also unmutes), `mute\|on\|off`, `wifi\|on\|off`, `airdrop\|off\|contacts\|everyone`, `dark\|on\|off`, `lock\|now`. The host maps each key to one fixed command; anything else is rejected. |
| `FOCUS\|<agent-id>` | An agent session card was tapped: bring the app or terminal showing that session to the front on the Mac. `agent-id` comes from `AGENT`. |
| `SETTING\|<key>\|<value>` | The user changed a tablet setting on the deck. The host should store it and send it back on the next connection. |
| `SCREEN\|on\|dim\|off` | Screen state changed (also sent right after `HELLO`). While `off`, the host sends only `PING`, plus `WAKE` and `STATUS` for a notice that should turn the screen on. |

## Mac → Deck

| Message | Meaning |
|---|---|
| `PING\|<token>` | Heartbeat, about 1/s. **If the deck hears nothing for 5 s it drops the connection** and shows "disconnected". |
| `CLOCK\|<time>\|<date>` | Text shown in the top bar, e.g. `CLOCK\|14:05\|Sat 21 Nov` |
| `TILE\|<slot>\|<label>\|<value>\|<detail>\|<level>` | Metric tile, slots 0–3. `level` is 0–100 and drives the bar under the value (`-1` = no bar). Home shows all four; Stats uses slot 0 CPU, 1 memory, 2 disk, 3 network for headline values. |
| `SAMPLE\|<key>\|<number>` | One history point for the Stats charts, about 1/s. The deck keeps the last 120 per key. Keys: `cpu`, `mem` (percent), `net_down`, `net_up` (bytes/s). |
| `BUTTON\|<slot>\|<id>\|<label>\|<rrggbb>[\|home]` | Defines button slot 0–11, shown on the Shortcuts page. With the `home` flag it is also pinned to the Home page (the first three flagged; if none is flagged, the first three buttons). An empty `id` clears the slot. |
| `ICON\|button\|<slot>\|<64 hex digits>` | Icon for button slot 0–11, sent right after its `BUTTON` (a new `BUTTON` clears the slot's icon). 16×16 pixels as 16 rows of 16 bits, 4 hex digits per row, the most significant bit is the leftmost pixel. The deck draws it above the label in the label's ink. |
| `AGENT\|<slot>\|<agent>\|<state>\|<title>\|<where>\|<since>[\|<detail>[\|<id>]]` | AI coding-agent session card, slots 0–5, shown on the Agents page in slot order (the host sorts: waiting, then working, then idle). `agent` is e.g. `Claude`, `Codex`, `opencode`, `Copilot` or `Qwen`; `state` is `working`, `waiting` (needs the user) or `idle`; `title` is the session name; `where` the project folder; `since` how long it has been in this state, preformatted (`45s`, `12m`, `3h`); `detail` what it is waiting for, if known; `id` a short token the deck sends back in `FOCUS`. An empty `agent` clears the slot. Sent about 1/s while the screen is on. With more sessions than slots, the host fills only slots 0–4 and the deck shows the rest as a "+N more" card. |
| `AGENTS\|<total>\|<waiting>\|<working>\|<idle>` | Counts of all agent sessions, shown or not: the top-bar "needs you" badge, the Home summary and "+N more". Sent with the `AGENT` slots. |
| `MAC\|battery\|<pct>\|<charging 0\|1>\|<source>\|<line>\|<detail>` | Mac battery for the Mac page: `source` e.g. `Power adapter`, `line` e.g. `Charging, 0:42 to full`, `detail` e.g. `Health 96%  197 cycles`. |
| `MAC\|sound\|<volume 0-100>\|<muted 0\|1>` | Output volume and mute. |
| `MAC\|wifi\|on\|off\|<detail>` · `MAC\|airdrop\|off\|contacts\|everyone` · `MAC\|dark\|on\|off` | The Mac page's switches. All `MAC` messages are sent every 2 s while the screen is on, and right after each `MACSET`. |
| `STATUS\|<text>[\|<seconds>]` | Short-lived message (toast) in the bottom bar, e.g. the result of an action. Shown for `seconds` (default 4, max 60), then the line clears. Use ~8 s for errors. |
| `SETTING\|<key>\|<value>` | Restore a tablet setting (the tablet has no persistent storage) |
| `WAKE` | Turn the screen on, e.g. for a notification |

### Tablet settings

| Key | Values | Default |
|---|---|---|
| `brightness` | 1–255 backlight level | 80 |
| `dim` | seconds of inactivity before dimming, `0` = never | 60 |
| `off` | seconds of inactivity before the screen turns off, `0` = never | 300 |
| `cpu` | `auto` (interactive governor, up to 1205 MHz) or `saver` (conservative, max 624 MHz) | auto |

Measured on the SM-T210 over a 250 mA USB port: brightness 255 ≈ −426 mA, 160 ≈ −190 mA, 80 ≈ −98 mA, 30 ≈ −56 mA, backlight off ≈ +78 mA (charging).

The host owns the content. Buttons, tiles and samples are sent by the Mac on every connection, so changing the deck's buttons never requires reflashing the tablet.

## Deck pages

| Page | Content |
|---|---|
| Home | an agent summary (counts and up to 4 sessions; tapping anywhere on it opens the Agents page), the CPU, memory and network tiles, and three pinned buttons; the deck returns here when the screen dims |
| Agents | one card per `AGENT` slot: state, time in that state, title, agent and project; tap to open the session on the Mac |
| Stats | CPU and memory charts (0–100 %), network chart (down/up, auto-scaled), disk meter |
| Shortcuts | all button slots in a 4×3 grid |
| Mac | the Mac's battery, sound (volume bar, mute, quieter, louder), Wi-Fi, AirDrop (off / contacts / everyone icons), Dark Mode and Lock Screen |
| Tablet | the tablet's battery, power, brightness and system readings and its local controls |

## Example session

```
deck → HELLO|macdeck/1|deck-1|1024x600
mac  → BUTTON|0|calculator|Calculator|63dab4
mac  → TILE|0|CPU|23%|8 cores|23
mac  → CLOCK|14:05|Sat 21 Nov
mac  → PING|1
deck → PONG|1
deck → PRESS|calculator
mac  → STATUS|Opened Calculator
```
