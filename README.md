# ESP32-FabGL-MSX Emulator (MSX1 / MSX2+)

## Demonstration

[![MSX Emulator on ESP32 - GRADIUS](https://img.youtube.com/vi/d_9KP-l-2iU/0.jpg)](https://www.youtube.com/watch?v=d_9KP-l-2iU)

*MSX1エミュレータ（SCCサウンド対応）で『グラディウス』を動作させている様子をYouTubeでご覧いただけます。*
*Watch GRADIUS running on the MSX1 emulator (with SCC sound) on YouTube.*

[日本語](#japanese) / [English](#english)

---

<a name="japanese"></a>

# 日本語

## 概要

ESP32とFabGLライブラリを使用したMSXエミュレータです（開発中）。次の2つのスケッチがあります。

| スケッチ | 機種 | 画面 |
|---|---|---|
| `MSX/MSX.ino` | MSX1（TMS9918A） | 320×200 |
| `MSX2/MSX2.ino` | MSX2+（ソニー HB-F1XDJ 相当、V9958。MSX2のBIOSならV9938として動作） | 512×212 |

起動メニュー、ファイル選択は両方で共通です。ファンクションキーは F12・F8 が共通で、ディスクの入れ替えなどは MSX1 と MSX2+ で割り当てが違います（「5. 操作ガイド」の表を参照）。

## 構成プロジェクト

1. **MSX Emulator (ESP32)**
   - FabGLベースのMSX本体エミュレータ（MSX1 / MSX2+）。
2. **Joystick Converter (STM32)**
   - アタリ仕様のジョイスティックをESP32-MSXEmulator仕様に変換するためのユニット。

### クレジットと謝辞

- **グラフィック/オーディオエンジン**: [Fabrizio Di Vittorio氏](https://github.com/fdivitto/FabGL)による FabGL を使用。
- **Z80 CPUコア**: Marat Fayzullin氏のコードをベースに改変。
- **VDPセクション（MSX1）**: David Latham氏のTMS9918Aコアをベースに、Marat Fayzullin氏が調整した実装を改変。
- **FM音源（YM2413）**: 岡崎光考氏の emu2413 を使用。

### 動作確認済み環境 / ビルド設定

- **ボード**: ESP32 Dev Module（TTGO VGA32）
- **ESP32 ボードマネージャ**: v2.0.5 (推奨)
- **使用ライブラリ**: FabGL v1.0.9
- **Arduino IDE 推奨設定**:
   - **PSRAM**: "Enabled" (必須)
   - **Partition Scheme**: MSX1 は "Default 4MB with SPIFFS" 等、MSX2 は "Huge APP" 等

### 免責事項・注意事項

- 本ソフトウェア内の各コアのライセンスは、FabGL、Marat Fayzullin氏 (fMSX License)、岡崎光考氏 (emu2413) の各ポリシーに従います。
- 本プロジェクトは開発中であり、不具合や最適化不足なコードが含まれる場合があります。
- 本ソフトウェアの使用によって生じたいかなる損害についても、開発者は一切の責任を負いません。

## 1. 本プロジェクトの目的と背景

本プロジェクトは、ESP32とFabGLライブラリを使用し、MSXエミュレータを実機に近い60FPSで動作させることを目的としています。
ESP32の実行リソースが限られているため、実機の完全な再現（100%の互換性）ではなく快適な動作環境の構築を目指しています。

> **開発・ライセンス・ローカライズに関するご注意**
>
> - ソースコードの殆どはAIで生成し、製作者はそれを切り貼りして構成しています。製作者自身もコードのすべてを完全に理解しているわけではありません。そのため、ソースコードの具体的な仕様やロジックに関する詳細な質問にはお答えいたしかねます。
> - キーボードの対応は日本語・米英語・ポルトガル語・スペイン語・イタリア語のレイアウトを用意していますが、これらはすべてAIにて生成したものであり日本語以外の環境では動作検証をしていません。
>
> ```cpp
> // Please select one of the following options.
> #define KEYBORD_MAP (KeyboardLayout::JAPANESE)
> //#define KEYBORD_MAP (KeyboardLayout::US_ENGLISH)
> //#define KEYBORD_MAP (KeyboardLayout::PORTUGUESE)
> //#define KEYBORD_MAP (KeyboardLayout::SPANISH)
> //#define KEYBORD_MAP (KeyboardLayout::ITALIAN)
> ```
>
> ご使用の環境にて不具合が生じた場合、ご自身でローカライズをお願いします。また、修正されたコードの公開を歓迎します。
>
> ライセンス関連は各 .ino 冒頭の文章を読み、了承したうえで本ソフトウエアをご使用ください。

## 2. システム構成

### MSX1

- **スロット構成**
   - Slot 0: メインBIOS
   - Slot 1: カートリッジ1（ゲームROM / SCC / なし）
   - Slot 2: カートリッジ2（ゲームROM / SCC / FM-PAC / なし）
   - Slot 3: RAM (64KB)
   - ディスクBIOS（DISK.ROM）は空いているスロット（2 → 1 の順）に入ります。スロット1・2の両方を使っているときだけ、スロット3を拡張スロットにして 3-1 に入れます（3-0 は RAM）。
- **音源**: PSG、SCC（コナミのSCCカートリッジ、ROMの無いSCCカートリッジ）、FM-PAC（YM2413）

### MSX2+

- **スロット構成**（HB-F1XDJ に準じる）
   - 0-0: BIOS + BASIC / 0-2: MSX-MUSIC（内蔵）
   - 1: カートリッジA / 2: カートリッジB（FM-PAC を選ぶと内蔵MSX-MUSICの代わりにここに入ります）
   - 3-0: RAM（メモリマッパー）/ 3-1: サブROM・漢字BASIC / 3-2: ディスクROM
- **音源**: PSG、SCC、MSX-MUSIC / FM-PAC（YM2413）

## 3. SDカードの構成とセットアップ

SDカードのルートに、MSX1 は `/MSX1`、MSX2+ は `/MSX2` のフォルダを作り、それぞれの中に以下を置きます（マーク3 は `/SMS`、PC-8801 は `/PC88`、X1 は `/X1` なので、1枚のSDカードに全機種のファイルを入れられます）。

### MSX1

```
/ (SD Root)
└── /MSX1
    ├── MSX.ROM         : MSX1本体の純正BIOS (32KB / 必須)
    ├── DISK.ROM        : ディスクインターフェースROM (16KB / FDD使用時に必須)
    ├── FMPAC.ROM       : FM-PAC のROM (16KB または 64KB / FM-PAC を使うとき)
    ├── FMPAC.PAC       : FM-PAC の SRAM の保存ファイル (自動で作られます)
    ├── JOYKEY.CFG      : Joy B1〜B3 の設定 (自動で作られます)
    ├── /ROM            : ゲームROMファイル (.rom) を格納（中にフォルダを作ってもよい）
    │   └── /SAVE       : ステートセーブデータ保存用フォルダ
    └── /DISK           : ディスクイメージファイル (.dsk) を格納（中にフォルダを作ってもよい）
```

### MSX2+

```
/ (SD Root)
└── /MSX2
    ├── MSX2P.ROM / MSX2PEXT.ROM : MSX2+ の BIOS とサブROM（無ければ MSX2.ROM / MSX2EXT.ROM、それも無ければ C-BIOS）
    ├── MSX2PMUS.ROM    : 内蔵 MSX-MUSIC の ROM
    ├── KNJBASIC.ROM    : 漢字BASIC（MSX2+ のみ）
    ├── KANJI.ROM       : 漢字ROM
    ├── DISK.ROM        : ディスクROM（ソニー型 7FF8h〜7FFFh）
    ├── FMPAC.ROM       : FM-PAC の ROM（あればスロット2の既定が FM-PAC）
    ├── FMPAC.PAC       : FM-PAC の SRAM の保存ファイル (自動で作られます)
    ├── JOYKEY.CFG      : Joy B1〜B3 の設定 (自動で作られます)
    ├── /ROM            : ゲームROMファイル (.rom)
    └── /DISK           : ディスクイメージファイル (.dsk)
```

ROMマッパーは自動判別しません。以下の識別子（半角）をファイル名に加えてください。

- Konami（SCCあり/なし両用） … `(K)`
- Konami SCC（実機どおりのアドレスのみ。メタルギア2など。MSX2のみ） … `(KS)`
- Ascii8KB … `(A8)` または `(A)`
- Ascii16KB … `(A16)`
- R-TYPE … `(R)`
- 32KB以下 … なし

> **重要：ROMに関する注意**
>
> - **BIOS**: MSX1 は純正BIOSを前提としています（C-BIOS非対応）。MSX2+ は純正BIOSが無ければ C-BIOS（カートリッジのみ）で起動します。
> - **DISK.ROM (MSX1)**: FDDを使用する場合、次のどちらかの形式のDISK.ROMが必要です。お使いのDISK.ROMに合わせて、MSX.ino の下記の行で切り替えてください。
>   - ソニー/フィリップス型（メモリマップド、FDCレジスタ 7FF8h〜7FFFh） → 下記を有効にする（初期設定）
>   - I/Oポート型（Microsol等、FDCレジスタ D0h〜D4h） → 下記をコメントアウトする
>
>   ```cpp
>   #define DISK_IF_MEMORY_MAPPED
>   ```
>
>   メモリマップド型を選んだときは、起動時のシリアルログに「`DiskROM: 7FF8h-7FFFh refs=`」が出ます。`refs=0` で WARNING が出た場合は、I/Oポート型のDISK.ROMの可能性があります。
>   松下（ナショナル）型（7FB8h〜7FBFh）には対応していません。
> - FDDを使用しない場合（MSX1）、下記をコメントアウトしてください。
>
>   ```cpp
>   #define USE_FDC
>   ```

## 4. 起動メニュー

電源を入れると起動メニューが出ます（MSX1 / MSX2+ 共通）。↑↓ で項目を選び、Enter で変更・起動します。

| 項目 | 内容 |
|---|---|
| Slot1 / Slot2 | カートリッジ。/MSX1/ROM・/MSX2/ROM の .rom ファイル、または `<NONE>`（なし）・`<SCC>`（ROMの無いSCCカートリッジ）・`<FM-PAC>`（Slot2のみ。FMPAC.ROM があるとき既定） |
| Disk BIOS | ディスクBIOSの ON / OFF（DISK.ROM があるとき） |
| Disk1 / Disk2 | ドライブ1 / 2 に入れるディスクイメージ（.dsk） |
| Mouse | `NONE` / `PORT 1` / `PORT 2`（PS/2マウスをそのジョイスティックポートにつなぐ） |
| Joy B1〜B3 | マウスをつないだポートのコントローラのボタン1〜3で押すキー（既定は SPACE / RETURN / ESC） |
| Run | 起動 |

起動メニューとファイル選択は、ポート1のコントローラ（アタリ仕様 → シリアル変換器の1P）でも操作できます：↑↓ = 選択（押し続けると連続）、←→ = 1画面送り、ボタン1 = 決定（Enter）、ボタン2 = 取り消し（ESC）。

- **Disk BIOS の既定**: ゲームROMだけで起動するときは自動で OFF になります（ワークエリアを取られて動かないゲームがあるため）。ディスクを入れたとき、ROMを挿さないとき（BASIC）は ON です。Enter で切り替えると以後は自動で変わりません。
- **`<SCC>`**: ROMの無いSCCカートリッジです。『SDスナッチャー』などサウンドカートリッジを使うソフト向けです（SCCのみ。RAMはありません）。
- **FM-PAC の SRAM**: 書き込みが止まって2秒たつと SD の `/MSX1/FMPAC.PAC`・`/MSX2/FMPAC.PAC` に自動で保存し、次の起動で読み込みます（openMSX などと同じ形式）。
- **マウス**: TTGO VGA32 の2つめの PS/2 端子につないだマウスを、選んだジョイスティックポートのMSXマウスとして使います（左ボタン = トリガA、右ボタン = トリガB）。
- **マウスのポートのコントローラ（JoyToKey）**: Mouse で `PORT 1` / `PORT 2` を選ぶと、そのポートはマウスが使うので、同じポート番号のコントローラ（PORT 1 なら 1P、PORT 2 なら 2P）はキーボードとして働きます。方向 = カーソルキー、ボタン1〜3 = Joy B1〜B3 のキーです（PC-88 版と同じ考え方）。Mouse が `NONE` のときは働かず、コントローラは普通のジョイスティックです。もう一方のポートのコントローラも普通のジョイスティックのままです。
- **Joy B1〜B3**: Enter を押すと行が黄色になり、次に押したキーがそのボタンに割り当たります（PC-88 版と同じ操作）。F6〜F12 など MSX に無いキーを押すと取り消し、その行で BackSpace を押すと `NONE`（何も押さない）に戻します。設定は SD の `/MSX1/JOYKEY.CFG`・`/MSX2/JOYKEY.CFG` に保存し、次の起動でも使います（前の版の1バイトのファイルはボタン3の設定として読みます）。

### ファイル選択画面

- フォルダは「名前/」で **水色**、選んでいる項目は **黄色** で表示します（全機種共通）。
- Enter でフォルダに入る、「../」か BackSpace で1つ上へ戻る、PageUp/PageDown で1画面送り、ESC で取り消し。

## 5. 操作ガイド（ファンクションキー）

> ディスクの入れ替えやステートセーブなどは、**[F12] で一時停止（Pause）した状態**で行います。

| キー | MSX1 | MSX2+ |
|---|---|---|
| F12 | 一時停止 / 再開 | 同じ |
| Shift + F12 | リセット | 同じ |
| F9（一時停止中） | **ドライブ1**のディスク入れ替え | なし |
| Shift + F9（一時停止中） | **ドライブ1**のディスクをSDへ書き戻し | なし |
| F10（一時停止中） | SDのステートをPSRAMへ読み込み（1〜5を選ぶ） | **ドライブ1（A:）**のディスク入れ替え |
| Shift + F10（一時停止中） | PSRAMのステートをSDへ書き出し（1〜5を選ぶ） | **ドライブ1（A:）**のディスクをSDへ書き戻し |
| F11（一時停止中） | クイックロード（1〜5を選ぶ） | **ドライブ2（B:）**のディスク入れ替え |
| Shift + F11（一時停止中） | クイックセーブ（1〜5を選ぶ） | **ドライブ2（B:）**のディスクをSDへ書き戻し |
| F8 | PSGをSCCの波形で鳴らす ON / OFF | 同じ |
| Shift + F8 | スプライトの横の制限（4個）を外す | なし |
| F6 | なし | Z80の速さ Normal → High → Super → Normal（画面下に0.5秒表示） |

- **MSX1 と MSX2+ の違い（注意）**
   - ディスクの入れ替えは、MSX1 が **F9**（ドライブ1のみ）、MSX2+ が **F10（ドライブ1）・F11（ドライブ2）** です。
   - F10・F11 は、MSX1 ではステートセーブ、MSX2+ ではディスクの入れ替えです（MSX2+ にステートセーブはありません）。
   - Shift + F8（スプライト制限の解除）は MSX1 のみ、F6（Z80の速さ）は MSX2+ のみです。
- ※ディスクの書き戻しにライトプロテクト機能はありません。オリジナルデータは別途保管してください。
- ※SCC対応ソフトの実行中は F8 を使わないでください。
- ※Shift + F8 を ON にすると衝突検出・5枚目検出が止まります。不具合が出る場合は OFF にしてください。
- ※F6 の速さは 1ライン（約63.6µs）に実行する Z80 のクロック数です（Normal 205 = 実機相当、High 270、Super 335）。処理落ちするゲームを速くしたいときに使います。速いとタイミングに頼るソフトが正しく動かないことがあります。

### ステートセーブ＆ロード（MSX1）

1. **PSRAM（一時保存）**: 電源を切ると消去されます
   - [Pause中に Shift + F11] ＋ [1～5] : クイックセーブ
   - [Pause中に F11] ＋ [1～5] : クイックロード
2. **SDカード（永続保存）**: 電源を切っても残ります
   - [Pause中に Shift + F10] ＋ [1～5] : PSRAMのデータをSDへ書き出し
   - [Pause中に F10] ＋ [1～5] : SDのデータをPSRAMへ読み込み
   - ※SDからロードしたデータを反映させるには、読み込み後に [F11] ＋ [1～5] を実行してください。

## 6. ジョイスティック（外部コントローラー）の接続

本プロジェクトでは、ESP32のシリアルポート（UART）を介して外部変換機から送信されるアタリ仕様コントローラーの信号をサポートしています（MSX1 / MSX2+ 共通）。

### 6-1. 接続仕様

- 通信規格: RS-232C シリアル通信
- ボーレート: 115200 bps
- 接続ポート: IO39
- 更新周期: 16.67ms (約59.94Hz)
- 対応ポート: MSX ジョイスティックポート 1 および ポート 2（起動メニューでマウスをつないだポートはマウスになります）

### 6-2. 外部変換器のデータフォーマット

シリアル経由で以下の2バイトバイナリデータを受信し、MSXのジョイスティック状態に反映します。
データは「1Pデータ → 2Pデータ」の順で連続送信されることを想定しています。

| Bit | 機能     | 内容                                 |
|-----|----------|--------------------------------------|
| b7  | ID       | 0 = Port A (1P) / 1 = Port B (2P)    |
| b6  | ボタン3  | 0:ON / 1:OFF (Active Low)。マウスのポートのコントローラのとき、起動メニューの Joy B3 で決めたキー |
| b5  | ボタン2  | 0:ON / 1:OFF                         |
| b4  | ボタン1  | 0:ON / 1:OFF                         |
| b3  | 右       | 0:ON / 1:OFF                         |
| b2  | 左       | 0:ON / 1:OFF                         |
| b1  | 下       | 0:ON / 1:OFF                         |
| b0  | 上       | 0:ON / 1:OFF                         |

<img src="JoyStickConnection.JPG" width="300">

基板にヘッダを半田付けし、端側のピン（白:3.3V、黒:GND、グレー:RX）に接続します。

## 7. キーボード操作マニュアル

本エミュレータは、PC用のPS/2キーボードをMSXのキーボードとしてエミュレートします。
現在は「日本語JIS配列」をベースに最適化されています。

### 基本キー割り当て

| PCキーボード側        | MSXキー側        |
|-----------------------|------------------|
| 半角/全角、カタカナ/ひらがな | かな (KANA)  |
| 変換 / 無変換         | GRAPH            |
| 左Alt / 右Alt         | GRAPH            |
| Esc                   | ESC              |
| Tab                   | TAB              |
| Ctrl                  | CTRL             |
| F1 ～ F5              | F1 ～ F5         |
| Insert / Delete       | INS / DEL        |
| Home                  | HOME / CLS       |
| End                   | SELECT           |
| PageUp / PageDown     | STOP             |
| Enter                 | RETURN           |
| Backspace             | BS               |

### テンキー

テンキーの数字および「`*`」「`/`」「`-`」「`+`」「`.`」は、MSXのテンキーとして動作します（Enter は RETURN）。

### 日本語以外のキーボード

`KEYBORD_MAP` で US_ENGLISH / PORTUGUESE（ブラジル ABNT2）/ SPANISH / ITALIAN を選ぶと、各国版（インターナショナル）MSX のキーマトリクスへ **キーの位置で** 割り当てます。各国版 MSX は同じ位置のキーが同じマトリクスにつながっていて、印字だけが国ごとに違うためです（例: L の右のキーは US の `;`、スペインの `Ñ`、ブラジルの `Ç`、イタリアの `ò` で、どれも行1 bit7）。表示される文字は、その国の MSX の BIOS（MSX.ROM）で決まります。

| PCキーボード側        | MSXキー側（各国版）         |
|-----------------------|-----------------------------|
| 右Alt (AltGr)         | CODE                        |
| 左Alt                 | GRAPH                       |
| `<` `>`（ISO キーボード） / ABNT2 の `/ ?` | DEAD（アクセント） |
| PageUp / PageDown     | STOP                        |

### エミュレータ特殊操作

F6・F8〜F12 はエミュレータの操作に使います（機種で割り当てが違います。「5. 操作ガイド」の表を参照）。

## 8. 開発者向け：キーボード・ローカライズ設計資料

本プロジェクトでは、PS/2のスキャンコードをMSXのキーマトリクス（Row/Bit）へ直接変換しています。
新しい言語レイアウトを追加したり、特定のキーを変更したりする場合は、以下の設計思想に従ってください。

### マッピングの構造

```cpp
key_map[スキャンコード] = 0xRB;
```

- 上位4bit (R) : MSXキーマトリクスの「行 (Row)」 0x0～0xA
- 下位4bit (B) : MSXキーマトリクスの「ビット (Bit)」 0x0～0x7
- 値が `0xFF` : 未割り当て（無効）
- E0 の付くキー（カーソル・Home・Insert・Delete・End・PageUp/Down・テンキーの `/` と Enter・右Alt・右Ctrl）は `key_map_e0[]` に同じ形で入れます。

（例）`key_map[0x5A] = 0x77;`
0x5A（PS/2のEnter）を、MSXマトリクスの Row 7, Bit 7 (RETURN) に割り当て。

### ソースファイル構成

- `initKeyMap(layout)` : 起動時に選択された言語に応じてマップを初期化します（`KeyboardSystem.hpp`、MSX1 / MSX2+ 共通）。
- `setCommonKeyMap()` : どの国でも同じキー（英字・数字・ファンクション・テンキー・カーソルなど）。
- `setJapaneseKeyMap()` : 日本語JIS配列 → 日本の MSX。
- `setUSKeyMap()` : US配列 → 各国版 MSX（位置で割り当て。右Alt = CODE）。
- `setPortugueseKeyMap()` : ブラジル ABNT2 配列 → ブラジルの MSX（US と同じ位置の割り当てに、ABNT2 だけのキーを追加）。
- `setSpanishKeyMap()` : スペイン語配列 → スペインの MSX（US と同じ位置の割り当て。Ñ は L の右 = 行1 bit7）。
- `setItalianKeyMap()` : イタリア語配列 → イタリアの MSX（US と同じ位置の割り当て。ò は L の右 = 行1 bit7、`<` `>` は DEAD の位置）。

### 修正・追加の手順

1. MSXのテクニカルデータブック等で「キーマトリクス表」を確認します。
2. 変更したいキーの Row と Bit を特定します。
3. 対応する言語の関数（`set...KeyMap`）内で、PCキーボードのスキャンコードに対して「`0x(Row)(Bit)`」を代入してください。

※ 行9・行10 はテンキーです（どの国の MSX でも同じ）。国ごとの文字のために使う行ではありません。

---

<a name="english"></a>

# English

## Overview

MSX emulators for ESP32 using the FabGL library (Work in Progress). There are two sketches:

| Sketch | Machine | Screen |
|---|---|---|
| `MSX/MSX.ino` | MSX1 (TMS9918A) | 320×200 |
| `MSX2/MSX2.ino` | MSX2+ (modelled on Sony HB-F1XDJ, V9958; runs as V9938 with an MSX2 BIOS) | 512×212 |

The boot menu and the file selector are shared by both. F12 and F8 are common; disk swapping and other keys differ between MSX1 and MSX2+ (see the table in "5. Function Keys").

## Included Projects

1. **MSX Emulator (ESP32)**
   - MSX main unit emulators based on FabGL (MSX1 / MSX2+).
2. **Joystick Converter (STM32)**
   - A unit for converting Atari-style joysticks for use with the ESP32 MSX Emulator.

### Credits & Acknowledgments

- **Graphics/Audio Engine**: [FabGL](https://github.com/fdivitto/FabGL) by Fabrizio Di Vittorio.
- **Z80 CPU Core**: Based on code by Marat Fayzullin.
- **VDP Section (MSX1)**: Based on TMS9918A core by David Latham, with adjustments by Marat Fayzullin.
- **FM Sound (YM2413)**: emu2413 by Mitsutaka Okazaki.

### Build Requirements & Settings

- **Board**: ESP32 Dev Module (TTGO VGA32)
- **ESP32 Board Manager**: v2.0.5 (Recommended)
- **Required Library**: FabGL v1.0.9
- **Arduino IDE Settings**:
   - **PSRAM**: "Enabled" (Required)
   - **Partition Scheme**: MSX1: "Default 4MB with SPIFFS" or similar, MSX2: "Huge APP" or similar

### Important Notes & License

- This project adheres to the licenses of FabGL, Marat Fayzullin (fMSX License) and Mitsutaka Okazaki (emu2413).
- This is a work in progress; it may contain bugs or unoptimized code.
- The developer is not responsible for any damage caused by using this software.

## 1. Project Purpose and Background

This project aims to run MSX emulators at 60 FPS using an ESP32 and the FabGL library. Due to the limited hardware resources of the ESP32, the goal is not a 100% perfect reproduction of the original machine, but rather the creation of a comfortable and playable environment.

> **Notes on Development, Licensing, and Localization**
>
> - Most of the code was generated by AI, and the developer has primarily assembled and integrated these parts. The developer does not fully understand every detail of the code. Therefore, we cannot answer technical questions regarding specific logic or internal specifications of the source code.
> - Keyboard layouts are available for Japanese, US English, Portuguese, Spanish, and Italian. However, these were all AI-generated and have not been tested in environments other than Japanese.
>
> ```cpp
> // Please select one of the following options.
> #define KEYBORD_MAP (KeyboardLayout::JAPANESE)
> //#define KEYBORD_MAP (KeyboardLayout::US_ENGLISH)
> //#define KEYBORD_MAP (KeyboardLayout::PORTUGUESE)
> //#define KEYBORD_MAP (KeyboardLayout::SPANISH)
> //#define KEYBORD_MAP (KeyboardLayout::ITALIAN)
> ```
>
> If you encounter issues in your specific environment, please perform your own localization. We welcome the public release of any corrected code.
>
> Regarding licensing, please read the text at the beginning of each .ino and agree to the terms before using this software.

## 2. System Configuration

### MSX1

- **Slot Configuration**
   - Slot 0: Main BIOS
   - Slot 1: Cartridge 1 (game ROM / SCC / none)
   - Slot 2: Cartridge 2 (game ROM / SCC / FM-PAC / none)
   - Slot 3: RAM (64KB)
   - The disk BIOS (DISK.ROM) goes into a free slot (2, then 1). Only when both slots 1 and 2 are used, slot 3 becomes an expanded slot and the DISK.ROM goes into 3-1 (3-0 is RAM).
- **Sound**: PSG, SCC (Konami SCC cartridges and a ROM-less SCC cartridge), FM-PAC (YM2413)

### MSX2+

- **Slot Configuration** (follows the HB-F1XDJ)
   - 0-0: BIOS + BASIC / 0-2: MSX-MUSIC (internal)
   - 1: Cartridge A / 2: Cartridge B (choosing FM-PAC puts it here instead of the internal MSX-MUSIC)
   - 3-0: RAM (memory mapper) / 3-1: Sub ROM + Kanji BASIC / 3-2: Disk ROM
- **Sound**: PSG, SCC, MSX-MUSIC / FM-PAC (YM2413)

## 3. SD Card Configuration and Setup

Create a `/MSX1` folder (MSX1) and a `/MSX2` folder (MSX2+) in the root of the SD card and put the files below inside them (Mark III uses `/SMS`, PC-8801 `/PC88` and X1 `/X1`, so one SD card can hold the files of every machine).

### MSX1

```
/ (SD Root)
└── /MSX1
    ├── MSX.ROM         : MSX1 Original BIOS (32KB / Required)
    ├── DISK.ROM        : Disk Interface ROM (16KB / Required for FDD)
    ├── FMPAC.ROM       : FM-PAC ROM (16KB or 64KB / for FM-PAC)
    ├── FMPAC.PAC       : FM-PAC SRAM save file (created automatically)
    ├── JOYKEY.CFG      : Joy B1-B3 settings (created automatically)
    ├── /ROM            : Game ROM files (.rom) (sub-folders allowed)
    │   └── /SAVE       : Folder for State Save data
    └── /DISK           : Disk Image files (.dsk) (sub-folders allowed)
```

### MSX2+

```
/ (SD Root)
└── /MSX2
    ├── MSX2P.ROM / MSX2PEXT.ROM : MSX2+ BIOS and sub ROM (falls back to MSX2.ROM / MSX2EXT.ROM, then C-BIOS)
    ├── MSX2PMUS.ROM    : Internal MSX-MUSIC ROM
    ├── KNJBASIC.ROM    : Kanji BASIC (MSX2+ only)
    ├── KANJI.ROM       : Kanji ROM
    ├── DISK.ROM        : Disk ROM (Sony type, 7FF8h-7FFFh)
    ├── FMPAC.ROM       : FM-PAC ROM (if present, Slot 2 defaults to FM-PAC)
    ├── FMPAC.PAC       : FM-PAC SRAM save file (created automatically)
    ├── JOYKEY.CFG      : Joy B1-B3 settings (created automatically)
    ├── /ROM            : Game ROM files (.rom)
    └── /DISK           : Disk Image files (.dsk)
```

ROM mappers are not detected automatically. Please add one of the following identifiers (half‑width) to the file name:

- Konami (with or without SCC) … `(K)`
- Konami SCC (strict addresses as on real hardware, e.g. Metal Gear 2; MSX2 only) … `(KS)`
- ASCII 8KB … `(A8)` or `(A)`
- ASCII 16KB … `(A16)`
- R-TYPE … `(R)`
- 32KB or smaller … none

> **Important: Notes on ROMs**
>
> - **BIOS**: MSX1 requires an original BIOS (C-BIOS is not supported). MSX2+ boots with C-BIOS (cartridges only) when no original BIOS is found.
> - **DISK.ROM (MSX1)**: To use the FDD, a DISK.ROM of one of the following types is required. Select the type that matches your DISK.ROM with the following line in MSX.ino:
>   - Sony/Philips type (memory-mapped, FDC registers at 7FF8h-7FFFh) → keep this line enabled (default)
>   - I/O port type (e.g. Microsol, FDC registers at D0h-D4h) → comment out this line
>
>   ```cpp
>   #define DISK_IF_MEMORY_MAPPED
>   ```
>
>   With the memory-mapped type, the serial log at startup shows "`DiskROM: 7FF8h-7FFFh refs=`". If it shows `refs=0` with a WARNING, your DISK.ROM is probably an I/O port type.
>   The Panasonic/National type (7FB8h-7FBFh) is not supported.
> - If you do not intend to use the FDD (MSX1), please comment out the following line:
>
>   ```cpp
>   #define USE_FDC
>   ```

## 4. Boot Menu

A boot menu appears at power-on (MSX1 and MSX2+). Select an item with Up/Down and press Enter to change it or to start.

| Item | Meaning |
|---|---|
| Slot1 / Slot2 | Cartridge: a .rom file in /MSX1/ROM or /MSX2/ROM, or `<NONE>`, `<SCC>` (ROM-less SCC cartridge), `<FM-PAC>` (Slot 2 only; default when FMPAC.ROM exists) |
| Disk BIOS | Disk BIOS ON / OFF (when DISK.ROM exists) |
| Disk1 / Disk2 | Disk images (.dsk) for drive 1 / 2 |
| Mouse | `NONE` / `PORT 1` / `PORT 2` (connects a PS/2 mouse to that joystick port) |
| Joy B1-B3 | Keys pressed by buttons 1-3 of the controller on the mouse port (default SPACE / RETURN / ESC) |
| Run | Start |

The boot menu and the file selector can also be operated with the port 1 controller (1P of the Atari-to-serial converter): Up/Down = select (repeats while held), Left/Right = page up/down, button 1 = Enter, button 2 = cancel (ESC).

- **Disk BIOS default**: turned OFF automatically when booting a game ROM only (some games fail when the disk BIOS takes work area). It is ON when a disk is selected or when no ROM is inserted (BASIC). Once you toggle it with Enter it is no longer changed automatically.
- **`<SCC>`**: a ROM-less SCC cartridge for software that uses a sound cartridge, such as SD Snatcher (SCC only; no RAM).
- **FM-PAC SRAM**: saved automatically to `/MSX1/FMPAC.PAC` / `/MSX2/FMPAC.PAC` on the SD card 2 seconds after the last write, and loaded at the next boot (same format as openMSX).
- **Mouse**: a mouse on the second PS/2 connector of the TTGO VGA32 works as an MSX mouse on the selected joystick port (left button = trigger A, right button = trigger B).
- **Controller on the mouse port (JoyToKey)**: when Mouse is `PORT 1` / `PORT 2`, that port is used by the mouse, so the controller with the same number (1P for PORT 1, 2P for PORT 2) works as a keyboard: directions = cursor keys, buttons 1-3 = the Joy B1-B3 keys (same idea as the PC-88 version). With Mouse = `NONE` it is off and the controller is a normal joystick. The controller of the other port always stays a normal joystick.
- **Joy B1-B3**: press Enter and the row turns yellow; the next key you press is assigned to that button (same operation as the PC-88 version). A key the MSX does not have (F6-F12, etc.) cancels; BackSpace on the row sets it back to `NONE`. The setting is saved to `/MSX1/JOYKEY.CFG` / `/MSX2/JOYKEY.CFG` on the SD card and used at the next boot (an old 1-byte file is read as the button 3 setting).

### File Selector

- Folders are shown as "name/" in **light blue**; the selected item is **yellow** (same on all machines).
- Enter opens a folder, "../" or BackSpace goes up, PageUp/PageDown scrolls a page, ESC cancels.

## 5. Function Keys

> Disk swapping and state saving are done while the emulator is **paused** (press [F12]).

| Key | MSX1 | MSX2+ |
|---|---|---|
| F12 | Pause / Resume | same |
| Shift + F12 | Reset | same |
| F9 (paused) | Swap the disk in **drive 1** | - |
| Shift + F9 (paused) | Write **drive 1** back to the SD card | - |
| F10 (paused) | Load a state from SD to PSRAM (select 1-5) | Swap the disk in **drive 1 (A:)** |
| Shift + F10 (paused) | Save a state from PSRAM to SD (select 1-5) | Write **drive 1 (A:)** back to the SD card |
| F11 (paused) | Quick load (select 1-5) | Swap the disk in **drive 2 (B:)** |
| Shift + F11 (paused) | Quick save (select 1-5) | Write **drive 2 (B:)** back to the SD card |
| F8 | PSG played with SCC waveforms ON / OFF | same |
| Shift + F8 | Remove the horizontal sprite limit (4 per line) | - |
| F6 | - | Z80 speed Normal → High → Super → Normal (shown for 0.5 s at the bottom) |

- **Differences between MSX1 and MSX2+ (note)**
   - Disk swapping is **F9** (drive 1 only) on MSX1, and **F10 (drive 1) / F11 (drive 2)** on MSX2+.
   - F10 / F11 are state save keys on MSX1 and disk swap keys on MSX2+ (MSX2+ has no state save).
   - Shift + F8 (sprite limit removal) is MSX1 only; F6 (Z80 speed) is MSX2+ only.
- There is no write-protect for disk write-back. Please keep backups of your original data.
- Do not use F8 with software that already supports SCC.
- Shift + F8 disables collision and 5th-sprite detection. Turn it off if it causes glitches.
- The F6 speed is the number of Z80 clocks executed per scan line (about 63.6 µs): Normal 205 (real speed), High 270, Super 335. Use it to speed up games that slow down; software that depends on timing may misbehave at higher speeds.

### State Save & Load (MSX1)

1. **PSRAM (Temporary)**: Data is lost when power is turned off
   - [Shift + F11] + [1-5] (While Paused): Quick Save
   - [F11] + [1-5] (While Paused): Quick Load
2. **SD Card (Permanent)**: Data persists after power-off
   - [Shift + F10] + [1-5] (While Paused): Export PSRAM data to SD Card
   - [F10] + [1-5] (While Paused): Import data from SD Card to PSRAM
   - *To apply data loaded from the SD card, you must execute a Quick Load ([F11] + [1-5]) after importing.*

## 6. Joystick (External Controller) Connection

This project supports Atari-standard controller signals sent from an external converter via the ESP32's serial port (UART) (MSX1 and MSX2+).

### 6-1. Connection Specifications

- Communication Standard: RS-232C Serial Communication
- Baud Rate: 115200 bps
- Connection Port: IO39 (RX)
- Update Cycle: 16.67ms (Approx. 59.94Hz)
- Supported Ports: MSX Joystick Port 1 and Port 2 (a port selected for the mouse in the boot menu becomes the mouse)

### 6-2. External Converter Data Format

The emulator receives the following 2-byte binary data via serial and maps it to the MSX joystick status. The data is expected to be transmitted continuously in the order of "1P Data -> 2P Data."

| Bit | Function   | Content                                 |
|-----|------------|-----------------------------------------|
| b7  | ID         | 0 = Port A (1P) / 1 = Port B (2P)       |
| b6  | Button 3   | 0:ON / 1:OFF (Active Low); on the mouse-port controller, the key set by Joy B3 in the boot menu |
| b5  | Button 2   | 0:ON / 1:OFF                            |
| b4  | Button 1   | 0:ON / 1:OFF                            |
| b3  | Right      | 0:ON / 1:OFF                            |
| b2  | Left       | 0:ON / 1:OFF                            |
| b1  | Down       | 0:ON / 1:OFF                            |
| b0  | Up         | 0:ON / 1:OFF                            |

<img src="JoyStickConnection.JPG" width="300">

Solder the headers to the board. Connect to the pins on the edge of the board (White: 3.3V, Black: GND, Grey: RX).

## 7. Keyboard Manual

This emulator maps a PC PS/2 keyboard to the MSX keyboard matrix. It is currently optimized based on the "Japanese JIS" layout.

### Default Key Mappings

| PC Keyboard           | MSX Key          |
|-----------------------|------------------|
| Hankaku/Zenkaku, Katakana/Hiragana | KANA |
| Henkan / Muhenkan     | GRAPH            |
| Left Alt / Right Alt  | GRAPH            |
| Esc                   | ESC              |
| Tab                   | TAB              |
| Ctrl                  | CTRL             |
| F1 - F5               | F1 - F5          |
| Insert / Delete       | INS / DEL        |
| Home                  | HOME / CLS       |
| End                   | SELECT           |
| PageUp / PageDown     | STOP             |
| Enter                 | RETURN           |
| Backspace             | BS               |

### Numeric Keypad

Numbers and "`*`", "`/`", "`-`", "`+`", "`.`" on the keypad work as the MSX numeric keypad (keypad Enter is RETURN).

### Non-Japanese Keyboards

With `KEYBORD_MAP` set to US_ENGLISH / PORTUGUESE (Brazilian ABNT2) / SPANISH / ITALIAN, keys are mapped **by position** onto the international MSX key matrix. On international MSX machines a key at a given position is wired to the same matrix bit in every country; only the printed character differs (e.g. the key right of L is `;` in the US, `Ñ` in Spain, `Ç` in Brazil and `ò` in Italy, all row 1 bit 7). The character produced is decided by that country's MSX BIOS (MSX.ROM).

| PC Keyboard           | MSX Key (international)     |
|-----------------------|-----------------------------|
| Right Alt (AltGr)     | CODE                        |
| Left Alt              | GRAPH                       |
| `<` `>` (ISO keyboards) / ABNT2 `/ ?` | DEAD (accent) |
| PageUp / PageDown     | STOP                        |

### Emulator Special Commands

F6 and F8-F12 control the emulator (assignments differ between machines; see the table in "5. Function Keys").

## 8. For Developers: Keyboard Localization Design

This project directly maps PS/2 scan codes to the MSX keyboard matrix (Row/Bit). Use the following design principles when adding new languages or modifying keys.

### Mapping Structure

```cpp
key_map[scan_code] = 0xRB;
```

- Upper 4 bits (R): MSX Matrix Row (0x0 - 0xA)
- Lower 4 bits (B): MSX Matrix Bit (0x0 - 0x7)
- Value `0xFF`: Unassigned (Disabled)
- Keys with the E0 prefix (cursor keys, Home, Insert, Delete, End, PageUp/Down, keypad `/` and Enter, Right Alt, Right Ctrl) go into `key_map_e0[]` in the same format.

(Example) `key_map[0x5A] = 0x77;`
Maps 0x5A (PS/2 Enter) to MSX Row 7, Bit 7 (RETURN).

### Source File Structure

- `initKeyMap(layout)` : Initializes the map based on the selected language (`KeyboardSystem.hpp`, shared by MSX1 and MSX2+).
- `setCommonKeyMap()` : Keys that are the same in every country (letters, digits, function keys, keypad, cursor keys...).
- `setJapaneseKeyMap()` : Japanese JIS layout → Japanese MSX.
- `setUSKeyMap()` : US layout → international MSX (positional; Right Alt = CODE).
- `setPortugueseKeyMap()` : Brazilian ABNT2 layout → Brazilian MSX (US positional mapping plus the ABNT2-only keys).
- `setSpanishKeyMap()` : Spanish layout → Spanish MSX (US positional mapping; Ñ is right of L = row 1 bit 7).
- `setItalianKeyMap()` : Italian layout → Italian MSX (US positional mapping; ò is right of L = row 1 bit 7, `<` `>` is the DEAD position).

### Steps to Modify/Add

1. Consult the "Keyboard Matrix Table" in an MSX technical manual.
2. Identify the Row and Bit for the key you wish to change.
3. Assign the "`0x(Row)(Bit)`" value to the PC scan code inside the relevant language function.

\* Rows 9 and 10 are the numeric keypad on every MSX model; they are not used for country-specific characters.
