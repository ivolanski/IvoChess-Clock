# IvoChess Clock

A DIY physical chess clock built on an ESP32-C6 (Seeed XIAO ESP32C6), with a
2.13" Waveshare e-paper display, a WS2812 LED strip, and battery power. It
shows your live chess.com game - player names/ratings, move count, clocks
that tick between updates, whose turn it is, and the result when the game
ends - without needing a phone or a PC nearby once it's set up.

## How it works

1. **Discovery** (`ChessApiFunctions`): polls `GET /service/play/games` on
   chess.com using your saved `PHPSESSID` cookie to find your active live
   game (Daily/correspondence games don't show up here - only real-time
   games). If that cookie has expired (a plain 401), it automatically
   renews the session using `CHESSCOM_REMEMBERME` and retries - see
   "Staying signed in" below.
2. **Live data** (`LiveGameClient` + `RSocketCodec`): once a game is found,
   opens a WebSocket straight to chess.com and speaks
   [RSocket](https://rsocket.io/about/protocol) over it - the same protocol
   chess.com's own web client uses - to stream live clocks, moves, and the
   final result. No polling once connected; it's a push stream.
3. **Display/LEDs** (`DisplayFunctions`, `LedFunctions`): render the current
   `ClockState` to the e-paper screen and the LED strip. Between real
   RSocket updates, the clock is extrapolated locally (from a fixed
   timestamp anchor, not by chaining decrements - see `ClockState.h`) so it
   still counts down smoothly.
4. **Config** (`AdminPortal`): a single web page, reachable either via a
   `IvoChess-Setup` hotspot (no WiFi configured yet) or your normal network
   IP / `ivochess.local` once connected. Holds WiFi credentials, your
   chess.com username and session cookie, LED colors, and display timing -
   all persisted in NVS (`Preferences`), survives reflashing.

`GameDataSource` is a thin abstraction in front of all this, so a future
data source (e.g. a physical DGT/ChessConnect board over Bluetooth) can be
added without touching the display/LED code.

## Building

**Board:** Seeed XIAO ESP32C6 (`esp32:esp32:XIAO_ESP32C6`)

**Required Partition Scheme** (Arduino IDE → Tools → Partition Scheme):
**"Huge APP (3MB No OTA/1MB SPIFFS)"**. The default scheme only gives the
app 1.2MB, and this sketch (e-paper driver + fonts + WebSockets +
ArduinoJson) sits right at that ceiling - it won't compile on the default
scheme. Nothing in this project uses OTA updates, so losing that slot costs
nothing.

**Required libraries** (Library Manager):
- GxEPD2 (ZinggJM)
- Adafruit GFX Library
- Adafruit BusIO
- Adafruit NeoPixel
- ArduinoJson (Benoit Blanchon)
- **WebSockets** (Markus Sattler / Links2004) - *not* ArduinoWebsockets, see
  the note below.

### Important: WebSockets library needs a one-line patch

This project depends on a small patch to the installed `WebSockets`
library's `WebSocketsClient.cpp` (`sendHeader()`): it must skip appending
its own default `User-Agent` header when the caller already supplied one.
Without the patch, the connection to chess.com drops right after every
`SETUP` frame with no useful error. See the top-of-file comment in
`LiveGameClient.cpp` for the exact bug, why it happens, and the patch
itself. **Redo this patch if the library gets reinstalled or updated** -
it's a change to files outside this repo (in your Arduino libraries
folder), so it won't survive a library manager update on its own.

## First-time setup

1. Flash the sketch. On first boot (no WiFi saved yet) it starts a
   `IvoChess-Setup` WiFi hotspot.
2. Connect to it, open `http://192.168.4.1/` (or just wait for the captive
   portal prompt).
3. Fill in your WiFi network, your chess.com `PHPSESSID` **and**
   `CHESSCOM_REMEMBERME` cookies (both from your browser's DevTools →
   Application → Cookies, while logged into chess.com - see below for why
   both), and your chess.com username. Save - it restarts to join your
   WiFi.
4. From then on, reachable at `http://ivochess.local/` (or its IP) for any
   further changes - LED colors, result screen duration, etc. Most
   settings apply immediately without a restart; only a WiFi network/
   password change triggers one.

## Staying signed in

`PHPSESSID` alone (what earlier versions of this project used) expires
periodically - the clock would eventually start reporting `HTTP 401` and
need a fresh cookie pasted in by hand. Chrome/Safari/etc never show you
this because a normal browser tab renews it silently: chess.com also sets
a much longer-lived `CHESSCOM_REMEMBERME` cookie, and any plain page load
(confirmed by testing: `GET /home`, *not* the `/service/...` API routes,
which don't participate in this) with a dead `PHPSESSID` but a still-valid
`CHESSCOM_REMEMBERME` gets a brand new `PHPSESSID` back via `Set-Cookie` -
no login form involved. `CHESSCOM_REMEMBERME` itself rotates to a new
token on every use (standard remember-me security practice - re-using an
old one fails), so `ChessApiFunctions.cpp` captures and persists *both*
cookies from that response, not just `PHPSESSID`.

`fetchActiveGame()` does this automatically: a 401/403 triggers one
renewal-and-retry before giving up. As long as `CHESSCOM_REMEMBERME`
itself doesn't get invalidated (logging out on chess.com, changing your
password, or the clock simply not being used for a very long time all
would), the clock should stay signed in indefinitely without you ever
touching the admin portal again. If it ever does report a session error
in `apiStatus`, both cookies need recapturing.

## Known limitations

- Only chess.com **live** games are supported (Daily/correspondence never
  appear on the discovery endpoint used here).
- Only tracks one shard/game at a time - if you somehow have more than one
  live game active, only the first one found is watched.
- True partial-window e-paper refresh (`updateGameClocksPartial` in
  `DisplayFunctions.cpp`) is implemented but disabled - it caused visual
  corruption on real hardware. Clocks currently update via periodic full
  refreshes instead (see `GAME_CLOCK_REFRESH_INTERVAL_MS`).
