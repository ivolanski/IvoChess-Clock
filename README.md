# IvoChess Clock

A DIY physical chess clock built on an ESP32-C6 (Seeed XIAO ESP32C6), with a
2.13" Waveshare e-paper display, a WS2812 LED strip, and battery power. It
shows your live chess.com or Lichess game — player names/ratings, move
count, clocks that tick between updates, whose turn it is, and the result
when the game ends — without needing a phone or a PC nearby once it's set
up. It's independent of any physical board or app: it talks straight to
chess.com's or Lichess's own servers, the same way the website does.

It also works over Bluetooth with [ChessConnect](https://chessconnect.de),
for anyone bridging a physical board that way — pick "DGT3000 BLE Gateway"
as the clock type and it shows up with no Wi-Fi login needed.

## Supported platforms

- **Chess.com** and **Lichess** — connected directly, no bridge needed.
- **Noctie.ai**, **Chessiverse**, **ChessDojo**, and **ChessDojo.club** — via ChessConnect.

## Compatible boards

IvoChess Clock doesn't pair with a physical board directly — it watches the
online game on chess.com or Lichess (or, via ChessConnect, the same game a
board is already feeding into one of those sites). So it works alongside
any board that gets its moves onto chess.com or Lichess, including:

- **Chessnut** — Air, Air+, Pro, Evo (Bluetooth, connects to chess.com directly or via ChessConnect)
- **DGT** — Pegasus, e-Board, Smart Board (chess.com's DGT Live Chess software, Lichess's Board API, or ChessConnect)
- **Millennium** — eONE, The King Performance, Chess Genius Exclusive, and other boards paired with the ChessLink module (via ChessConnect or Lichess's Board API)
- **Certabo** — all boards (an open protocol, supported by both Lichess's Board API and ChessConnect)
- **TabuTronic** — all boards, including Cerno (via ChessConnect)
- **ChessUp** — ChessUp and ChessUp 2 (via ChessConnect)
- **Square Off** — Pro and NEO with the BillyGate adapter (via ChessConnect)
- **House of Staunton** — Sensory Board (via ChessConnect)
- **iChessOne** (via ChessConnect)
- **Wuxing** (via ChessConnect)

Board support tends to expand over time — if yours isn't listed, it's worth
checking chess.com, Lichess, or [ChessConnect](https://chessconnect.de)
directly, since IvoChess Clock will work alongside it either way.

**Full docs — hardware list, soldering guide, 3D-printed case, firmware
build/flash instructions, daily use, webadmin, and how to connect
chess.com, Lichess, or ChessConnect — live on the project site:**
**[ivochess.ivolanski.com](https://ivochess.ivolanski.com)**

This project is an independent, community-built effort and is not
affiliated with or endorsed by Chess.com or Lichess.

Questions or contact: ivochess@atomicmail.io
