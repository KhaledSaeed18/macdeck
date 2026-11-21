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
| `SETTING\|<key>\|<value>` | The user changed a tablet setting on the deck. The host should store it and send it back on the next connection. |
| `SCREEN\|on\|dim\|off` | Screen state changed (also sent right after `HELLO`). While `off`, the host sends only `PING`, plus `WAKE` and `STATUS` for a notice that should turn the screen on. |

## Mac → Deck

| Message | Meaning |
|---|---|
| `PING\|<token>` | Heartbeat, about 1/s. **If the deck hears nothing for 5 s it drops the connection** and shows "disconnected". |
| `CLOCK\|<time>\|<date>` | Text shown in the top bar, e.g. `CLOCK\|14:05\|Sat 21 Nov` |
| `TILE\|<slot>\|<label>\|<value>\|<detail>\|<level>` | Metric tile, slots 0–3. `level` is 0–100 and drives the bar under the value (`-1` = no bar). Home shows all four; Stats uses slot 0 CPU, 1 memory, 2 disk, 3 network for headline values. |
| `SAMPLE\|<key>\|<number>` | One history point for the Stats charts, about 1/s. The deck keeps the last 120 per key. Keys: `cpu`, `mem` (percent), `net_down`, `net_up` (bytes/s). |
| `BUTTON\|<slot>\|<id>\|<label>\|<rrggbb>[\|home]` | Defines button slot 0–11, shown on the Shortcuts page. With the `home` flag it is also pinned to the Home page (the first four flagged; if none is flagged, the first four buttons). An empty `id` clears the slot. |
| `AGENT\|<slot>\|<agent>\|<state>\|<title>\|<where>\|<since>[\|<detail>]` | AI coding-agent session card, slots 0–5, shown on the Agents page in slot order (the host sorts: waiting, then working, then idle). `agent` is e.g. `Claude` or `Codex`; `state` is `working`, `waiting` (needs the user) or `idle`; `title` is the session name; `where` the project folder; `since` how long it has been in this state, preformatted (`45s`, `12m`, `3h`); `detail` what it is waiting for, if known. An empty `agent` clears the slot. Sent about 1/s while the screen is on. |
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
| Home | the four tiles and four pinned buttons; the deck returns here when the screen dims |
| Agents | one card per `AGENT` slot: state, time in that state, title, agent and project |
| Stats | CPU and memory charts (0–100 %), network chart (down/up, auto-scaled), disk meter |
| Shortcuts | all button slots in a 4×3 grid |
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
