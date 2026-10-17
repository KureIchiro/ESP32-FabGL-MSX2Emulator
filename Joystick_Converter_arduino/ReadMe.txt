Arduino版 アタリ仕様ジョイスティック - ESP32 UART変換器
======================================================

概要
----
Arduino Mega 2560で2台のアタリ仕様コントローラーを読み取り、
ESP32-MSXエミュレータ用の2バイトデータをUARTで送信します。

STM32版のUSB HID機能は含みません。このスケッチはArduino Mega 2560を
対象としており、Serial1 (TX1) から115200 bpsで送信します。

配線
----
すべてのコントローラー入力と連射スイッチ入力は INPUT_PULLUP です。
各スイッチは対応する入力ピンとGNDの間に接続してください。
押す / ONにする操作でGNDに接続されます。

コントローラー1:
  上: D22    下: D23    左: D24    右: D25
  ボタン1: D26  ボタン2: D27  ボタン3: D28
  連射SW1: D29  連射SW2: D30  連射SW3: D31

コントローラー2:
  上: D32    下: D33    左: D34    右: D35
  ボタン1: D36  ボタン2: D37  ボタン3: D38
  連射SW1: D39  連射SW2: D40  連射SW3: D41

UART:
  Arduino Mega TX1 (D18) -> ESP32 IO39 (RX)
  Arduino GND          -> ESP32 GND

重要: Arduino Mega 2560のTX1は5Vロジックです。ESP32の入力は5V耐性が
ないため、TX1とIO39の間に抵抗分圧回路またはロジックレベル変換器を
必ず入れ、IO39へ約3.3Vを入力してください。5Vを直接接続しないでください。
3.3V系ボードへ移植する場合も、使用するボードのUART電圧を確認してください。

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

Arduino IDEで Arduino Mega 2560 を選択して、このフォルダ内の
Joystick_Converter_arduino.ino を書き込んでください。


English
-------

Arduino Atari-Style Joystick to ESP32 UART Converter
====================================================

Overview
--------
This sketch reads two Atari-style controllers on an Arduino Mega 2560 and
sends their two-byte data frame to the ESP32 MSX emulator over UART.

USB HID functionality from the STM32 version is not included. The sketch
targets Arduino Mega 2560 and transmits at 115200 bps through Serial1 (TX1).

Wiring
------
All controller and turbo-switch inputs use INPUT_PULLUP. Connect each switch
between its assigned input pin and GND. A pressed button or enabled switch
connects the input to GND.

Controller 1:
  Up: D22       Down: D23    Left: D24    Right: D25
  Button 1: D26  Button 2: D27  Button 3: D28
  Turbo SW 1: D29  Turbo SW 2: D30  Turbo SW 3: D31

Controller 2:
  Up: D32       Down: D33    Left: D34    Right: D35
  Button 1: D36  Button 2: D37  Button 3: D38
  Turbo SW 1: D39  Turbo SW 2: D40  Turbo SW 3: D41

UART:
  Arduino Mega TX1 (D18) -> ESP32 IO39 (RX), through level conversion
  Arduino GND             -> ESP32 GND

Important: Arduino Mega 2560 TX1 uses 5V logic, while ESP32 inputs are not
5V-tolerant. Always use a resistor voltage divider or logic-level shifter
between TX1 and IO39 so the ESP32 receives about 3.3V. Never connect 5V
directly to an ESP32 input. When porting to a 3.3V board, check its UART
logic voltage as well.

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

In Arduino IDE, select Arduino Mega 2560 and upload
Joystick_Converter_arduino.ino from this folder.
