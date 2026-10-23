# vimate-s3-lcd

The deck on Vimate's S3 LCD boards: the 3.5″ one (the board Vimate ships its GENU AI firmware on): a 320×480 ST77922
touch panel that the case holds sideways, so the screen is 480×320 landscape. Your agents in a 3×2
grid, each with its critter, hat and status line; scroll for more, tap one for its card. Same UI as
[t4s3-panel](../t4s3-panel), laid out for the smaller screen.

It subscribes to every agent with all fields. Answering, focusing and interrupting need scope **act**:

```sh
agentctl devices approve vimate-s3-35lcd-<id>
agentctl devices scope vimate-s3-35lcd-<id> act
```

## The board

ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM), on USB-C through the S3's own USB serial.

| part | pins |
|---|---|
| ST77922 panel, QSPI on SPI2's IO_MUX pins | CS 10, CLK 12, D0 11, D1 13, D2 14, D3 9; no reset line |
| backlight, PWM, active high | 41 |
| touch, built into the ST77922, I²C 0x55 (16-bit registers) | SDA 38, SCL 39, RST 48, INT 47 (unused) |
| ES8311 codec on the same I²C bus (0x18), amp enable | 1 (left alone) |

The panel's init sequence is the one Vimate's firmware sends. Two things it needs that the T4-S3 doesn't:
it garbles writes narrower than a full panel row, so LVGL draws into a full framebuffer in PSRAM and
`flush()` sends whole rows (turned 90° on the way); and the touch report has to be read whole, or the
controller keeps returning the last touch. `FLIP` in `src/main.c` turns the screen 180° (USB on the left).

## The S3 2.5″ board

The same firmware also runs Vimate's smaller board: build `-e vimate25`. It stands upright: a 2×3 grid on 240×320, fonts a size smaller.

| part | pins |
|---|---|
| ILI9341 panel, SPI3, 40 MHz | MOSI 11, CLK 12, CS 10, D/C 46; no reset line |
| backlight, PWM, active high | 45 |
| FT6236 touch, I²C 0x38 | SDA 16, SCL 15, RST 18 |

Espressif's default ILI9341 sequence as in Vimate's firmware, BGR, MADCTL MX (portrait, not mirrored;
Vimate uses MV for landscape) and inversion on (it's an IPS panel); the touch needs no turning. The board picks its drivers by `BOARD_VIMATE_25` (`src/display.h`).

## Build and flash

```sh
pio run -d firmware/devices/vimate-s3-lcd -t upload --upload-port /dev/cu.usbmodem<…>
```

This replaces Vimate's firmware. Back up the whole flash first if you want it back
(`esptool.py read_flash 0 0x1000000 vimate.bin`, then `write_flash 0 vimate.bin` to restore).

If `~/.platformio` is a symlink, PlatformIO's ESP-IDF builder fails with "Multiple ways to build the same
target"; run it with `PLATFORMIO_CORE_DIR` set to the real path.

`pio run -e vimate-debug` builds the debug version (`!snap`, `!tap`, `!log`, see `src/debug.h`).

## Bluetooth

The board also speaks Deck Link over BLE (advertised as `AgentsDeck-<id>`, a Nordic-UART-style service,
`src/ble.c`); while a Mac is connected there it uses BLE, otherwise USB. On the Mac,
`tools/ble_bridge.py` hands the board to the daemon as a serial port (a pty behind `~/.agents-deck-ble`):

```sh
pip install bleak pyobjc-framework-Cocoa pyinstaller
(cd ../../../backend && go build -o ../firmware/devices/vimate-s3-lcd/tools/bin/agentctl ./cmd/agentctl)  # bundled
cd firmware/devices/vimate-s3-lcd/tools && pyinstaller AgentsDeckBLE.spec   # AgentsDeckBLE.app
open dist/AgentsDeckBLE.app            # allow Bluetooth once; log: ~/Library/Logs/AgentsDeckBLE.log
agentctl link add ~/.agents-deck-ble
```

On a Mac without the deck the app installs its bundled agentctl (with the pty fix) and registers the
port itself; its menu also approves the connected screen and puts open Claude sessions on it.
The app lives in the menu bar (`tools/ble_menu.py`): click it for the boards around and pick one; the
pick is kept in `~/.agents-deck-ble.json`. **Ngắt kết nối** lets go of the board so another Mac can take
it (one Mac at a time). macOS only lets an app with a Bluetooth usage string use it, hence the app. A rebuilt app has a new ad-hoc
signature and loses the permission: `tccutil reset BluetoothAlways dev.agents-deck.ble-bridge` and allow
it again. No pairing: anyone nearby can connect, but only an approved board on the deck gets data.
