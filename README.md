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

**Full docs — hardware list, soldering guide, 3D-printed case, firmware
build/flash instructions, daily use, webadmin, and how to connect
chess.com, Lichess, or ChessConnect — live on the project site:**
**[ivochess.ivolanski.com](https://ivochess.ivolanski.com)**

This project is an independent, community-built effort and is not
affiliated with or endorsed by Chess.com or Lichess.

Questions or contact: ivochess@atomicmail.io
