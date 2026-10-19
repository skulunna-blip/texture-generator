# Texture Generator (Qt) + TG Blender Link

A desktop **PBR texture generator** written in **C++ / Qt Widgets**, paired with a
**Blender extension** that pushes generated maps straight into your open Blender
project over a localhost socket.

> 简体中文：这是一套贴图生成工具。桌面软件（`app/`）用 C++/Qt 写，从一张图片生成
> PBR 贴图（基础色/法线/凹凸/粗糙度/金属度/AO）；配套的 Blender 扩展（`extension/`）
> 接收贴图并自动搭材质，免安装绿色版见 Releases 页。

---

## What's inside / 仓库结构

```
texture-generator/
├── app/                       # Qt desktop app (C++17, Qt 6, CMake/Ninja)
│   ├── src/                   #   sources
│   ├── CMakeLists.txt
│   ├── *_main.cpp             #   headless regression tests (build/tg_*.exe)
│   ├── Launch-Texture-Generator.bat
│   ├── models/                #   Depth Anything V2 Small (ONNX) + its licence
│   ├── third_party/onnxruntime/  # ONNX Runtime (MIT), loaded at run time
│   ├── _get_depth_model.ps1   #   downloads the model + runtime (~61 MB)
│   └── _get_realesrgan.ps1    #   optional Real-ESRGAN upscaler downloader
├── extension/                 # TG Blender Link — official Blender extension
│   ├── blender_manifest.toml  #   extension metadata (id, version, support)
│   ├── __init__.py            #   socket server + material builder
│   ├── icon.png / featured.png
│   └── UPLOAD_COPY.md         #   platform upload copy (EN/中文)
├── dist/                      # ready-to-use packages
│   ├── tg_blender_link.zip               # Blender 4.2+ extension zip
│   └── TextureGeneratorQt-portable.zip   # 免安装绿色版 — built by _pack_portable.ps1,
│                                         #   not in git; grab it from Releases
└── LICENSE                    # GPL-3.0-or-later
```

## Desktop app features

- Dark tool UI, 中文/English switchable.
- 1K / 2K / 4K output, optional square crop, optional seamless tiling
  (offset-blend + wrap-aware derived maps).
- Material decomposition engines: Classic Fast, Enhanced Detail (multi-scale DoG
  + Sobel, default), and **AI Material Decomposition** — monocular depth from
  Depth Anything V2 drives the height map, so the normal, AO and bump maps
  describe real geometry instead of luminance patterns.
- **Remove Lighting (去光影)** — flattens the highlights, shadows and colour cast
  a photo arrived with, so the base color is closer to true albedo. The de-lit
  image becomes `base_color.png` itself, and every other map is derived from it.
  Works with any engine; when the AI engine is on, the depth map guides it so
  shading does not bleed across object outlines.
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

The AI depth feature needs two files that are too large to keep in git. Fetch
them once with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File _get_depth_model.ps1
```

That downloads the Depth Anything V2 Small checkpoint into `app/models/` and
ONNX Runtime into `app/third_party/onnxruntime/`. Rebuild afterwards and CMake
copies both next to the executable. Without them the app still builds and runs —
the AI engine entry just greys itself out and everything falls back to the local
engines.

## Install the Blender extension

Blender **4.2+**: *Edit → Preferences → Get Extensions → Install from Disk* →
pick `dist/tg_blender_link.zip`, enable **TG Blender Link**. A **TG** sidebar tab
appears in the 3D Viewport showing the listening state. Report bugs on the
[Issues](https://github.com/skulunna-blip/texture-generator/issues) page.

## Portable app (免安装)

Download `TextureGeneratorQt-portable.zip` from the
[Releases](https://github.com/skulunna-blip/texture-generator/releases) page. It
runs on any 64-bit Windows PC with no install — unzip and double-click
`TextureGeneratorQt.exe`. Blender must be installed for the Blender-link
feature; Real-ESRGAN (for upscaling) is optional, see `tools/README.txt` inside
the zip.

To build the package yourself, run `_pack_portable.ps1` after building; it
writes the zip into `dist/`. The zip is not tracked in git — it is a build
artifact that changes every release.

## Third-party components

| Component | Version | Licence | Why it is bundled |
|---|---|---|---|
| [Depth Anything V2](https://github.com/DepthAnything/Depth-Anything-V2) (Small) | v2 | Apache-2.0 | Monocular depth for the AI height/normal/AO path |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) | 1.20.1 | MIT | Runs the depth model; loaded at run time via `QLibrary` |
| [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (ncnn-vulkan) | v0.2.5.0 | BSD-3-Clause | Optional upscaler, downloaded separately by the user |

Licence texts ship with the distribution: `app/models/LICENSE-DepthAnythingV2-Apache-2.0.txt`
and `app/third_party/onnxruntime/LICENSE` (both copied into the portable package
as well). All three are compatible with this project's GPL-3.0-or-later.

> Only the **Small** Depth Anything checkpoint may be redistributed here.
> Upstream licenses Small under Apache-2.0 but licenses Base, Large and Giant
> under CC-BY-NC-4.0, which forbids commercial use. Do not swap in a larger
> checkpoint.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
