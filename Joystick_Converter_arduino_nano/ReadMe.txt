Arduino Nano版 アタリ仕様ジョイスティック - ESP32 UART変換器
===========================================================

対象
----
ATmega328P搭載の5V Arduino Nano (Classic) 用です。
STM32版のUSB HID機能は含みません。

入力ピンを全て使うため、NanoのD0 (RX) も入力として使います。
スケッチ起動後はUART受信機能を無効にし、D0を通常の入力として読み取ります。
通信出力はハードウェアUARTのD1 (TX) を使用します。

配線
----
方向、ボタン、連射SWの入力は通常 INPUT_PULLUP で、スイッチを対応入力と
GNDの間に接続します。押す / ONにする操作でGNDに接続されます。

コントローラー1:
  上: D2      下: D3      左: D4      右: D5
  ボタン1: D6  ボタン2: D7  ボタン3: D8
  連射SW1: D9  連射SW2: D10 連射SW3: D11

コントローラー2:
  上: D12     下: D13     左: A0      右: A1
  ボタン1: A2  ボタン2: A3  ボタン3: A4
  連射SW1: A5  連射SW2: D0  連射SW3: A6

NanoのA6はアナログ入力専用で、内部プルアップを使えません。連射SW3は
A6とGNDの間に接続し、A6と5Vの間に外付け10kΩプルアップ抵抗を接続してください。

UART:
  Nano D1 (TX) -> 抵抗分圧またはレベル変換器 -> ESP32 IO39 (RX)
  Nano GND     -> ESP32 GND

重要: 5V NanoのTX信号をESP32へ直接接続しないでください。ESP32の入力は
5V耐性がありません。抵抗分圧またはロジックレベル変換器でIO39への信号を
約3.3Vにしてください。

D0はUSBシリアル変換器にも接続されるため、スケッチを書き込むときは
コントローラー2の連射SW2を押さないでください。D0がGNDに固定されていると
書き込みに失敗することがあります。

通信仕様
--------
  ボーレート: 115200 bps
  送信周期: 約12ms
  送信順: コントローラー1、コントローラー2 (各1バイト)

各バイトのビット配置 (Active Low):
  b7: ID (0=コントローラー1、1=コントローラー2)
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

Arduino Nano Atari-Style Joystick to ESP32 UART Converter
=========================================================

Target Board
------------
This sketch is for the 5V Arduino Nano Classic with an ATmega328P.
USB HID functionality from the STM32 version is not included.

All Nano pins are needed, so D0 (RX) is also used as an input. The sketch
disables UART reception at startup and reads D0 as a regular input.
UART output uses the hardware serial TX pin, D1.

Wiring
------
Direction, button, and turbo-switch inputs use INPUT_PULLUP. Connect each
switch between its assigned input and GND. Pressing a button or enabling a
switch connects the input to GND.

Controller 1:
  Up: D2         Down: D3    Left: D4    Right: D5
  Button 1: D6   Button 2: D7   Button 3: D8
  Turbo SW 1: D9   Turbo SW 2: D10  Turbo SW 3: D11

Controller 2:
  Up: D12        Down: D13   Left: A0    Right: A1
  Button 1: A2   Button 2: A3   Button 3: A4
  Turbo SW 1: A5   Turbo SW 2: D0   Turbo SW 3: A6

Nano A6 is analog-input-only and has no internal pull-up. Connect a 10k ohm
external pull-up resistor between A6 and 5V, and connect Turbo SW 3 between
A6 and GND.

UART:
  Nano D1 (TX) -> ESP32 IO39 (RX), through a resistor divider or level shifter
  Nano GND     -> ESP32 GND

Important: Do not connect the 5V Nano TX signal directly to the ESP32. ESP32
inputs are not 5V-tolerant. Use a resistor divider or logic-level shifter to
provide about 3.3V at IO39.

D0 is also connected to the USB-to-serial adapter. When uploading the sketch,
make sure Controller 2 Turbo SW 2 connected to D0 is off. If D0 is held at
GND, uploading may fail.

Communication Protocol
----------------------
  Baud rate: 115200 bps
  Transmit interval: approximately 12 ms
  Byte order: Controller 1, then Controller 2 (one byte each)

Bit layout for each byte (active low):
  b7: ID (0=Controller 1, 1=Controller 2)
  b6: Button 3   b5: Button 2   b4: Button 1
  b3: Right      b2: Left      b1: Down      b0: Up
  ON=0, OFF=1

Connect a turbo switch to GND to enable rapid fire for its corresponding
button. The rapid-fire toggle interval is approximately 40 ms. Button 2
toggles in the opposite phase from Buttons 1 and 3 to match the STM32 version.

In Arduino IDE, select Arduino Nano and upload
Joystick_Converter_arduino_nano.ino from this folder.
