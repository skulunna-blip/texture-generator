# TG Blender Link — 上架文案（Upload Copy）

> ⚠️ **两个 Description 是两个不同字段，别混**。上传全程有两处都叫 Description：

| 字段 | 填在 | 限制 | 风格 |
|---|---|---|---|
| **GitHub 仓库描述** | github.com/new 建仓时 | **≤350 字符**（超长报错 `Description may not be longer than 350 characters`） | 一句话简介，展示在仓库名下方 |
| **扩展平台 Description** | extensions.blender.org 上传表单 | 无确认短限制，可填长 | 功能 + 用法 + 兼容性的完整说明（中英双版） |

其余要用到的字段：**tagline**（在 manifest 里，平台校验 ≤64 字符、结尾不能是标点）、**Release notes (Initial Version)**、**support 链接**（必须 URL，GitHub issues 最方便）、**icon.png / featured.png**（本目录已生成）。

---

## 1. GitHub 仓库描述（≤350 字符，一句话）

建一个新仓库（如 `skulunna-blip/texture-generator`，软件+插件都放这个仓库）时填入：

> Blender extension that receives PBR texture maps from the Texture Generator desktop app over a localhost socket and builds a material in your current scene — no new Blender instance needed. Requires Blender 4.2+.

字符数约 224，安全。

---

## 2. 扩展平台 Description（完整版，中英双版）

直接粘贴到 extensions.blender.org 的 Description 文本框：

---

### EN

**TG Blender Link** receives PBR texture maps from the **Texture Generator** desktop app and builds a complete material in your *current* Blender project — no new Blender instance, no manual file import.

**How it works**

The app sends the generated maps over a loopback socket (`127.0.0.1:9876`) to the already-open Blender. The request is acknowledged instantly and the material is built on Blender's main thread as soon as it is free, so a busy viewport or a long script never stalls the export. Each import creates a principled-BSDF material (base color, normal, bump, roughness, metallic, AO) grouped into labeled color frames, then applies it to all selected mesh objects — or to a spawned preview sphere when the scene is empty.

**Features**

- Live import into the currently open Blender session (keep your scene, selection and viewport).
- Fire-and-forget delivery — the app never waits on Blender.
- Auto material setup with Epic Bridge style colored frame groups.
- Applies to your selection; falls back to active object / preview sphere.
- Sidebar status panel (listening state, pending queue, startup errors) and a one-click connection test.

**Compatibility**

- Blender 4.2 or newer (extension format).
- Windows / macOS / Linux — requires the Texture Generator desktop app on the same machine.

**License**: GPL-3.0-or-later.

---

### 中文

**TG Blender Link** 把贴图生成器（Texture Generator）桌面应用生成的 PBR 贴图实时送进**当前已打开的 Blender 工程**，并自动搭好完整材质——无需手动导入文件，也无需启动新的 Blender 实例。

**工作方式**

应用通过本地回环端口（`127.0.0.1:9876`）把生成的贴图发给已打开的 Blender。请求即时确认，材质在 Blender 主线程空闲时自动搭建，所以视口拖拽或长时间脚本运行都不会卡住导出。每次导入都会基于基础色、法线、凹凸、粗糙度、金属度和 AO 贴图创建 principled-BSDF 材质，并按 Epic Bridge 风格归入带颜色标注的分组框，然后应用到当前所有选中的网格物体；场景为空时会自动创建一个预览球。

**功能**

- 实时导入当前打开的 Blender 会话（场景、选中、视口全部保留）。
- 异步投递：应用从不等待 Blender 执行。
- 自动搭建材质，参考 Epic Bridge 的彩色分组框导出方式。
- 应用到当前选中，无选中时作用于激活物体或预览球。
- 侧栏状态面板（监听状态、待处理队列、启动错误）和「测试连接」按钮。

**兼容性**

- 需要 Blender 4.2 及以上（官方扩展格式）。
- Windows / macOS / Linux，需与贴图生成器桌面应用同机运行。

**许可证**：GPL-3.0-or-later。

---

## 3. Release notes（Initial Version）

> **v1.1.0 — Initial release**
>
> - Live socket bridge between the Texture Generator desktop app and the current Blender session.
> - Fire-and-forget delivery: never blocks on Blender, survives a busy viewport.
> - Automatic principled-BSDF material with colored frame grouping (Epic Bridge style).
> - Applies to selected meshes / active object / preview sphere fallback.
> - Status panel + connection test.
> - Requires Blender 4.2+ and the Texture Generator app.

---

## 4. Support 链接

平台要求 support 是一个 URL（邮箱通常不行）。manifest 里已写好：

```
support = "https://github.com/skulunna-blip/texture-generator/issues"
```

⚠️ 这个仓库还没建。点下面的链接建一个空仓库（不带 README），我这边已经帮你把本地 git 仓库初始化好、提交好，你建好后跑两条命令就能 push（见仓库根 `_git_publish.ps1`）。或者你也可以先填 Blender Artists 论坛帖子的地址临时顶上，审核要求有 bug tracker。

---

## 5. manifest 状态（已就绪）

`extension/blender_manifest.toml` 已改成与 fill-select 一致的作者信息，**不再是占位符**：

```toml
maintainer = "lunna-sku <skulunna@gmail.com>"
support    = "https://github.com/skulunna-blip/texture-generator/issues"
website    = "https://github.com/skulunna-blip/texture-generator"
copyright  = ["2026 lunna-sku"]
```

⚠️ **`id = "tg_blender_link"` 发布后不可改**（改 = 全新扩展），确认后再提交。

---

## 6. 目前唯一的剩余步骤 = 网页上传

其余全部我这边已就绪（manifest/README/LICENSE/zip/图标/文案）。唯一需要你亲自做的是官方网页上传——因为它要**你的账号登录 + 两步验证**，我无法代替：
