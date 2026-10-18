Arduino Nano版 アタリ仕様ジョイスティック - ESP32 UART変換器 (1コントローラー専用)
==============================================================================

対象
----
ATmega328P搭載の5V Arduino Nano (Classic) 用です。
コントローラー1台のみに対応します (コントローラー2は非対応)。
STM32版のUSB HID機能は含みません。

配線
----
方向、ボタン、連射SWの入力は INPUT_PULLUP で、スイッチを対応入力と
GNDの間に接続します。押す / ONにする操作でGNDに接続されます。

  上: D2      下: D3      左: D4      右: D5
  ボタン1: D6  ボタン2: D7  ボタン3: D8
  連射SW1: D9  連射SW2: D10 連射SW3: D11

D0、D12、D13、A0〜A7は未使用です。

UART:
  Nano D1 (TX) -> 抵抗分圧またはレベル変換器 -> ESP32 IO39 (RX)
  Nano GND     -> ESP32 GND

重要: 5V NanoのTX信号をESP32へ直接接続しないでください。ESP32の入力は
5V耐性がありません。抵抗分圧またはロジックレベル変換器でIO39への信号を
約3.3Vにしてください。

通信仕様
--------
  ボーレート: 115200 bps
  送信周期: 約12ms
  送信内容: コントローラー1の状態を1バイト

バイトのビット配置 (Active Low):
  b7: ID (常に0 = コントローラー1)
  b6: ボタン3   b5: ボタン2   b4: ボタン1
  b3: 右         b2: 左         b1: 下         b0: 上
  ON=0、OFF=1

連射SWをGNDに接続すると対応ボタンの連射が有効になります。
連射の切り替え周期は約40msです。ボタン2はSTM32版との動作互換のため、
ボタン1・3と反対の位相で切り替わります。

Arduino IDEで「Arduino Nano」を選択して、このフォルダ内の
Joystick_Converter_arduino_nano.ino を書き込んでください。


English
-------

Arduino Nano Atari-Style Joystick to ESP32 UART Converter (1-Controller Version)
================================================================================

Target Board
------------
This sketch is for the 5V Arduino Nano Classic with an ATmega328P.
It supports a single controller only (Controller 2 is not supported).
USB HID functionality from the STM32 version is not included.

Wiring
------
Direction, button, and turbo-switch inputs use INPUT_PULLUP. Connect each
switch between its assigned input and GND. Pressing a button or enabling a
switch connects the input to GND.

  Up: D2         Down: D3    Left: D4    Right: D5
  Button 1: D6   Button 2: D7   Button 3: D8
  Turbo SW 1: D9   Turbo SW 2: D10  Turbo SW 3: D11

D0, D12, D13, and A0-A7 are unused.

UART:
  Nano D1 (TX) -> ESP32 IO39 (RX), through a resistor divider or level shifter
  Nano GND     -> ESP32 GND

Important: Do not connect the 5V Nano TX signal directly to the ESP32. ESP32
inputs are not 5V-tolerant. Use a resistor divider or logic-level shifter to
provide about 3.3V at IO39.

Communication Protocol
----------------------
  Baud rate: 115200 bps
  Transmit interval: approximately 12 ms
  Payload: one byte containing the Controller 1 state

Bit layout of the byte (active low):
  b7: ID (always 0 = Controller 1)
  b6: Button 3   b5: Button 2   b4: Button 1
  b3: Right      b2: Left      b1: Down      b0: Up
  ON=0, OFF=1

Connect a turbo switch to GND to enable rapid fire for its corresponding
button. The rapid-fire toggle interval is approximately 40 ms. Button 2
toggles in the opposite phase from Buttons 1 and 3 to match the STM32 version.

In Arduino IDE, select Arduino Nano and upload
Joystick_Converter_arduino_nano.ino from this folder.
