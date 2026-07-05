# M5DOOM

[中文](README.md)

This is a DOOM / PrBoom port for **M5Stack Tab5**, built with ESP-IDF.

## Status

- Target: M5Stack Tab5
- ESP-IDF: tested with v5.5.1
- Display: ST7121 / MIPI DSI
- Input: touch screen + Tab5 Keyboard
- WAD: `doom1-cut.wad` is included in the project

## Build

Enter the Tab5 project directory:

```powershell
cd src\Tab5DOOM
idf.py -B build -D IDF_TARGET=esp32p4 build
```

## Flash

```powershell
idf.py -B build -p COMx flash
```

The `flash` target writes both the firmware and the `doom1-cut.wad` data partition.

## Controls

Tab5 Keyboard:

- `W` / `↑`: move forward
- `S` / `↓`: move backward
- `A` / `←`: turn left
- `D` / `→`: turn right
- `E` / Space: use
- `F` / `Ctrl` / Enter: fire / confirm
- `Shift`: run
- Hold `Alt` + left/right: strafe left/right
- `Tab`: map
- `P`: pause
- `Esc`: menu / back
- `1`-`9` / `0`: switch weapon

Basic touch-screen virtual controls are also available.

## Note

The BSP display support for Tab5 is not fully reliable. This project includes the local display components required by Tab5.
