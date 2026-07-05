# M5DOOM

[English](README_EN.md)

这是一个给 **M5Stack Tab5** 使用的 DOOM / PrBoom 移植项目，基于 ESP-IDF。

## 状态

- 目标设备：M5Stack Tab5
- ESP-IDF：已在 v5.5.1 下编译运行
- 屏幕：ST7121 / MIPI DSI
- 输入：触摸屏 + Tab5 Keyboard
- WAD：项目内带 `doom1-cut.wad`

## 编译

进入 Tab5 工程目录：

```powershell
cd src\Tab5DOOM
idf.py -B build -D IDF_TARGET=esp32p4 build
```

## 烧录

```powershell
idf.py -B build -p COMx flash
```

`flash` 会同时烧录固件和 `doom1-cut.wad` 数据分区。

## 按键

Tab5 Keyboard：

- `W` / `↑`：前进
- `S` / `↓`：后退
- `A` / `←`：向左转视角
- `D` / `→`：向右转视角
- `E` / 空格：使用
- `F` / `Ctrl` / 回车：开火 / 确认
- `Shift`：跑
- 按住 `Alt` + 左右方向：左右平移
- `Tab`：地图
- `P`：暂停
- `Esc`：菜单 / 返回
- `1`-`9` / `0`：切换武器

触摸屏也保留了基础虚拟按键，可用于简单操作。

## 说明

BSP 对 Tab5 屏幕支持不完全可靠，本项目已经包含 Tab5 所需的本地屏幕组件。
