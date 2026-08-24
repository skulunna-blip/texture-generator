# Texture Generator (Qt) + TG Blender Link

A desktop **PBR texture generator** written in **C++ / Qt Widgets**, paired with a
**Blender extension** that pushes generated maps straight into your open Blender
project over a localhost socket.

> 简体中文：这是一套贴图生成工具。桌面软件（`app/`）用 C++/Qt 写，从一张图片生成
> PBR 贴图（基础色/法线/凹凸/粗糙度/金属度/AO）；配套的 Blender 扩展（`extension/`）
> 接收贴图并自动搭材质，免安装绿色版见 `dist/`。

---

## What's inside / 仓库结构

```
texture-generator/
├── app/                       # Qt desktop app (C++17, Qt 6, CMake/Ninja)
│   ├── src/                   #   sources
│   ├── CMakeLists.txt
│   ├── *_main.cpp             #   headless regression tests (build/tg_*.exe)
│   ├── Launch-Texture-Generator.bat
│   └── _get_realesrgan.ps1    #   optional Real-ESRGAN upscaler downloader
├── extension/                 # TG Blender Link — official Blender extension
│   ├── blender_manifest.toml  #   extension metadata (id, version, support)
│   ├── __init__.py            #   socket server + material builder
│   ├── icon.png / featured.png
│   └── UPLOAD_COPY.md         #   platform upload copy (EN/中文)
├── dist/                      # ready-to-use packages
│   ├── TextureGeneratorQt-portable.zip   # 免安装绿色版 (unzip & run, Windows)
│   └── tg_blender_link.zip               # Blender 4.2+ extension zip
└── LICENSE                    # GPL-3.0-or-later
```

## Desktop app features

- Dark tool UI, 中文/English switchable.
- 1K / 2K / 4K output, optional square crop, optional seamless tiling
  (offset-blend + wrap-aware derived maps).
- Material decomposition engines: Classic Fast, Enhanced Detail (multi-scale DoG
  + Sobel, default), and an optional AI back-end slot.
- Styles: Realistic and Anime/Cel-shaded (hard posterize + RGB Sobel outline).
- 3D material-ball preview with normal-only / bump-only isolation views.
- Blender import: live into the current session via `tg_blender_link`
  (port 9876), or by launching a new Blender instance.

## Build the app

```bash
cmake -S app -B app/build -G Ninja
cmake --build app/build
```

Requires Qt 6 (Widgets/Network/Concurrent/OpenGL), a C++17 compiler, Ninja.
`app/*_main.cpp` are headless regression targets (`tg_pitch_test`, `tg_style_check`, …).

## Install the Blender extension

Blender **4.2+**: *Edit → Preferences → Get Extensions → Install from Disk* →
pick `dist/tg_blender_link.zip`, enable **TG Blender Link**. A **TG** sidebar tab
appears in the 3D Viewport showing the listening state. Report bugs on the
[Issues](https://github.com/skulunna-blip/texture-generator/issues) page.

## Portable app (免安装)

`dist/TextureGeneratorQt-portable.zip` runs on any 64-bit Windows PC with no
install — unzip and double-click `TextureGeneratorQt.exe`. Blender must be
installed for the Blender-link feature; Real-ESRGAN (for upscaling) is optional,
see `tools/README.txt` inside the zip.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
