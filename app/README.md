# Texture Generator Qt

Desktop texture generator in `C++ + Qt Widgets`.

## 功能

- 深色工具风格界面，中/英双语切换
- 1K / 2K / 4K 输出
- 可选 1:1 裁剪
- 可选无缝边缘（offset-blend + wrap-aware 派生图）
- 生成 `base color`、`bump`、`normal`、`metal`、`roughness`、`AO`、`alpha`
- 可调 `bump` / `normal` 强度，实时低分辨率预览
- **材质分解引擎**：
  - 经典快速 / 本地启发式（Classic Fast）
  - 增强细节 / 本地增强（Enhanced Detail，多尺度 DoG + Sobel，默认）
  - AI 材质分解 / 外接后端（AiBridge，未安装）
- **贴图风格**：
  - 真实 / 物理材质（Realistic，原样输出）
  - 动漫 / 二次元（Anime / Cel-Shaded：逐通道硬 posterize 平涂 + RGB 三通道 Sobel 描边 + 饱和度提升，roughness 整体压低以呈现光洁质感）
- Blender 导入：
  - 优先发送到当前运行的 Blender（`tg_blender_link` 插件，端口 `9876`）
  - 否则启动本地 Blender 并执行生成的 Python 脚本

## 3D 材质球预览

- GL 3.3 Core，PBR 着色（GGX + 法线/凹凸 + 粗糙度/金属度/AO）
- 默认正面摆放，空闲时缓慢自转；拖动即定格精确旋转（与 Blender 同方向：拖下看顶），松手恢复自转
- GL 诊断日志：`%TEMP%\tg_preview_gl.log`

## 构建与测试

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

可执行测试目标（`build/tg_*.exe`，无头可跑）：

| 目标 | 验证内容 |
|------|----------|
| `tg_seam_check` | 无缝管线：边缘差收敛（offset-blend 后边界一致） |
| `tg_style_check` | 动漫风格：posterize 平涂 / 描边 / 色相保持 / roughness 压低 |
| `tg_pitch_test` | 材质球交互：拖下看顶、360° 无洞、松手恢复自转 |
| `tg_rotate_sweep` | 全角度旋转扫描：无背剔洞 |
| `tg_preview_sanity` | GL 预览路径：亮度均值 |

## 项目入口

- [CMakeLists.txt](/C:/Users/hasee/Documents/Codex/2026-08-20/blender-claude/outputs/texture-generator-qt/CMakeLists.txt)

主要文件：

- [main.cpp](/C:/Users/hasee/Documents/Codex/2026-08-20/blender-claude/outputs/texture-generator-qt/src/main.cpp)
- [MainWindow.cpp](/C:/Users/hasee/Documents/Codex/2026-08-20/blender-claude/outputs/texture-generator-qt/src/MainWindow.cpp)
- [TextureGenerator.cpp](/C:/Users/hasee/Documents/Codex/2026-08-20/blender-claude/outputs/texture-generator-qt/src/TextureGenerator.cpp)
- [BlenderBridge.cpp](/C:/Users/hasee/Documents/Codex/2026-08-20/blender-claude/outputs/texture-generator-qt/src/BlenderBridge.cpp)
- [MaterialPreviewWidget.cpp](/C:/Users/hasee/Documents/Codex/2026-08-20/blender-claude/outputs/texture-generator-qt/src/MaterialPreviewWidget.cpp)

## Blender 连接桥插件

- 插件名：`tg_blender_link`
- 位置：`C:\Users\hasee\AppData\Roaming\Blender Foundation\Blender\5.0\scripts\addons\tg_blender_link\`
- 已写入 `userpref.json` 自动启用。**若导入仍新开实例：重启 Blender 一次**（运行中的旧实例没加载插件、不监听 9876）。
