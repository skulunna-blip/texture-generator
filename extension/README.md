# TG Blender Link

Receives PBR texture maps from the **Texture Generator** desktop app and builds a
material in your **current** Blender project — no need to launch a new Blender
instance or import files by hand.

*TG Blender Link 是贴图生成器桌面应用（Texture Generator）的 Blender 接收端插件：把应用生成的 PBR 贴图通过本地端口实时送进当前打开的 Blender 工程并自动搭好材质，无需手动导入文件或启动新的 Blender。*

---

## Features / 功能

- **Live import into the current session** — the app talks to the already-open Blender over `127.0.0.1:9876`, so you keep your scene, selection and viewport. *(实时导入当前工程：应用通过本地端口连接已打开的 Blender，场景、选中、视图全部保留。)*
- **Fire-and-forget delivery** — the app never waits on Blender. The request is acknowledged instantly and the material is built on Blender's main thread as soon as it is free, so a busy viewport or a long script cannot stall the export. *(异步投递：应用不等 Blender 执行，请求即时确认，主线程空闲后自动搭材质——视图拖拽或长脚本运行不会卡住导出。)*
- **Auto material setup** — each import builds a principled-BSDF material from the base color, normal, bump, roughness, metallic and AO maps, grouped into labeled color frames (Epic Bridge style). *(自动搭材质：由基础色/法线/凹凸/粗糙度/金属度/AO 贴图自动构建 principled-BSDF 材质，并归入带颜色标注的分组框，参考 Epic Bridge 的导出方式。)*
- **Applies to your selection** — all selected mesh objects get the material; falls back to the active object, or spawns a preview sphere when the scene is empty. *(应用到当前选中：所有选中的网格物体都会被赋予材质；无选中时作用于激活物体，空场景时自动创建一个预览球。)*
- **Status panel** — a sidebar panel shows whether the link is listening, the pending queue depth, and any startup errors. *(状态面板：侧栏面板实时显示监听状态、待处理请求数、启动错误。)*
- **Test button** — "测试连接" checks the server state from inside Blender. *(测试按钮：可在 Blender 内一键检测连接服务是否在运行。)*

---

## Installation / 安装

The extension is distributed as a Blender 4.2+ extension (`.zip`).

*本扩展以 Blender 4.2+ 官方扩展（.zip）形式分发。*

1. Download the `.zip` from the extension platform (or the store page).
2. In Blender, open **Edit → Preferences → Get Extensions**.
3. Press the **down-arrow** button next to *Official* / *Community* repos, or the **Install from Disk** button, and pick the downloaded `.zip`.
4. Enable **TG Blender Link**.
5. Check the **TG** sidebar tab in the 3D Viewport — it should show *监听 127.0.0.1:9876*.

> Requires Blender **4.2 or newer** (`blender_version_min = "4.2.0"`).
> *需要 Blender 4.2 及以上版本。*

---

## Usage / 使用方法

1. Keep Blender open with this extension enabled.
2. Generate textures in the **Texture Generator** desktop app.
3. Click the app's **发送到当前 Blender** (Send to Blender) button.
4. A PBR material is created and applied to your selected mesh objects (or the preview sphere).
5. Watch the **TG** panel for status — queued requests appear as *待处理 N 个请求*.

*1. 保持 Blender 打开并启用本插件；2. 在贴图生成器桌面应用里生成贴图；3. 点击应用的「发送到当前 Blender」按钮；4. 材质自动创建并应用到选中的网格物体（或预览球）上；5. 在 TG 面板查看状态——排队请求会显示「待处理 N 个请求」。*

### Network layout / 网络说明

| Item | Value |
|---|---|
| Address | `127.0.0.1` (loopback only — never exposed to the network) |
| Port | `9876` |
| Protocol | JSON line terminated by a NUL byte: `{"type":"execute","code":"...", "strict_json":true}\0` |
| Payload cap | 16 queued requests (oldest dropped first) |

### Troubleshooting / 排查

- **Panel shows 未监听 (not listening)** — another Blender instance may already hold port 9876; close it, or restart the addon. *(面板显示「未监听」：可能另一个 Blender 实例占用了 9876 端口，关闭它或重启插件。)*
- **Exec errors** are written to `%TEMP%\tg_blender_link_error.log`. *(脚本执行错误会写入 `%TEMP%\tg_blender_link_error.log`。)*

---

## Notes / 说明

- Loopback-only socket; no network exposure.
- The generated material script runs on Blender's main thread via `bpy.app.timers` — safe to call from the app even while Blender is busy.
- The addon does not save files; it only builds in-memory materials from the texture paths sent by the app.
- The app must be on the same machine as Blender.

---

## File structure / 文件结构

```
tg_blender_link/
├── blender_manifest.toml   ← extension metadata (manifest)
├── __init__.py             ← socket server, timer executor, operator, panel
├── README.md
└── LICENSE                 ← GPL-3.0-or-later
```

## License / 许可证

GPL-3.0-or-later. See [LICENSE](LICENSE).
