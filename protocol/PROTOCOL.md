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

## Mac → Deck

| Message | Meaning |
|---|---|
| `PING\|<token>` | Heartbeat, about 1/s. **If the deck hears nothing for 5 s it drops the connection** and shows "disconnected". |
| `CLOCK\|<time>\|<date>` | Text shown in the top bar, e.g. `CLOCK\|14:05\|Sat 21 Nov` |
| `TILE\|<slot>\|<label>\|<value>\|<detail>\|<level>` | Metric tile, slots 0–3. `level` is 0–100 and drives the bar under the value (`-1` = no bar). |
| `BUTTON\|<slot>\|<id>\|<label>\|<rrggbb>` | Defines button slot 0–5. An empty `id` clears the slot. |
| `STATUS\|<text>` | One-line message in the bottom bar, e.g. the result of an action |

The host owns the layout. Buttons and tiles are defined by the Mac on every connection, so changing the deck's buttons never requires reflashing the tablet.

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
