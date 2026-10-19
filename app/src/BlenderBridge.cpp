#include "BlenderBridge.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QThread>

namespace
{
QString pythonEscaped(const QString& text)
{
    QString escaped = QDir::fromNativeSeparators(text);
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return escaped;
}

QString normalizedPath(const QString& path)
{
    return QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
}

bool hasMeaningfulAlpha(const QImage& image)
{
    if (image.isNull()) {
        return false;
    }

    for (int y = 0; y < image.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) < 250) {
                return true;
            }
        }
    }
    return false;
}
}

BlenderBridge::BlenderBridge(QObject* parent)
    : QObject(parent)
    , m_availablePaths(detectBlenderInstallations())
    , m_blenderPath(pickDefaultBlenderPath(m_availablePaths))
{
}

bool BlenderBridge::blenderExists() const
{
    return !m_blenderPath.isEmpty() && QFile::exists(m_blenderPath);
}

bool BlenderBridge::isMcpOnline() const
{
    // Probe is called from the UI thread (refreshBlenderStatus), so keep the
    // wait short: a localhost connect either succeeds immediately or is
    // refused in milliseconds when nothing listens.
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), 9876);
    return socket.waitForConnected(200);
}

QString BlenderBridge::blenderPath() const
{
    return m_blenderPath;
}

QStringList BlenderBridge::availableBlenderPaths() const
{
    return m_availablePaths;
}

void BlenderBridge::setBlenderPath(const QString& path)
{
    if (path.isEmpty()) {
        return;
    }

    const QString normalized = normalizedPath(path);
    if (!QFile::exists(normalized)) {
        return;
    }

    if (!m_availablePaths.contains(normalized, Qt::CaseInsensitive)) {
        m_availablePaths.prepend(normalized);
    }
    m_blenderPath = normalized;
}

BlenderBridge::ImportResult BlenderBridge::importMaterial(const TextureResult& result) const
{
    const bool liveSession = isMcpOnline();
    const QString script = buildMaterialScript(result, liveSession);

    if (liveSession) {
        ImportResult liveResult = importViaMcp(script);
        // Blender was reachable but may have been busy running an operation
        // (viewport recompute, modal operator, ...) when the timer was due. A
        // single retry after a short pause covers that transient stall.
        if (!liveResult.ok && liveResult.mode != QStringLiteral("offline")) {
            QThread::msleep(500);
            liveResult = importViaMcp(script);
        }
        if (liveResult.ok) {
            return liveResult;
        }
        // The live session was up but did not answer. If Blender quit while we
        // waited, launching a fresh instance is reasonable. If it is merely
        // busy, silently opening a NEW Blender would create a fresh project the
        // user did not ask for — surface the busy error instead so they can
        // retry on the current session.
        if (liveResult.message.contains(QStringLiteral("closed the connection"))) {
            if (blenderExists()) {
                return launchBlender(script, result.outputDirectory);
            }
        }
        return liveResult;
    }

    if (blenderExists()) {
        return launchBlender(script, result.outputDirectory);
    }

    return {false, QStringLiteral("Blender was not found and no live session is connected."), QStringLiteral("missing")};
}

QStringList BlenderBridge::detectBlenderInstallations() const
{
    QStringList found;

    auto add = [&found](const QString& candidate) {
        if (candidate.isEmpty()) {
            return;
        }
        const QString normalized = normalizedPath(candidate);
        if (QFile::exists(normalized) && !found.contains(normalized, Qt::CaseInsensitive)) {
            found.append(normalized);
        }
    };

    // 1. The installer records where it put Blender. Tried first because it is
    //    the only source that works regardless of drive or folder layout. Both
    //    registry views are read (a 32-bit host exposes its keys under
    //    WOW6432Node) and HKCU is included for per-user installs.
    const QStringList registryRoots = {
        QStringLiteral("HKEY_LOCAL_MACHINE"),
        QStringLiteral("HKEY_CURRENT_USER")
    };
    const QStringList registryKeys = {
        QStringLiteral("SOFTWARE\\BlenderFoundation\\Blender"),
        QStringLiteral("SOFTWARE\\WOW6432Node\\BlenderFoundation\\Blender")
    };
    for (const QString& root : registryRoots) {
        for (const QString& key : registryKeys) {
            QSettings blenderKey(root + QLatin1Char('\\') + key, QSettings::NativeFormat);
            const QStringList versions = blenderKey.childGroups();
            for (const QString& version : versions) {
                const QString installDir = blenderKey.value(version + QStringLiteral("/InstallDir")).toString();
                if (!installDir.isEmpty()) {
                    add(installDir + QStringLiteral("/blender.exe"));
                }
            }
        }
    }

    // 2. Every "Blender <version>" folder under the standard program
    //    directories. This used to name four versions explicitly, so a 4.3 or a
    //    3.6 install in the default location was invisible; any version found
    //    is offered now and the UI decides which ones it can link to.
    QStringList programRoots;
    for (const char* name : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"}) {
        const QString value = qEnvironmentVariable(name);
        if (!value.isEmpty() && !programRoots.contains(value, Qt::CaseInsensitive)) {
            programRoots.append(value);
        }
    }
    for (const QString& root : programRoots) {
        const QDir foundation(root + QStringLiteral("/Blender Foundation"));
        const QStringList versions = foundation.entryList({QStringLiteral("Blender *")},
                                                          QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& version : versions) {
            add(foundation.absoluteFilePath(version + QStringLiteral("/blender.exe")));
        }
        // Steam keeps its own copy outside Blender Foundation.
        add(root + QStringLiteral("/Steam/steamapps/common/Blender/blender.exe"));
    }

    // 3. Whatever is reachable through PATH.
    add(QStandardPaths::findExecutable(QStringLiteral("blender")));

    // 4. The folder layout the builds on this machine use. On any other machine
    //    these paths simply do not exist, which is why they are last rather
    //    than the only source.
    const QStringList known = {
        QStringLiteral("D:/blender5.1/blender-5.0.1-windows-x64/blender.exe"),
        QStringLiteral("D:/bl511/blender-5.1.0-windows-x64/blender.exe"),
        QStringLiteral("D:/blender4.5/blender-4.5.5-windows-x64/blender.exe"),
        QStringLiteral("D:/blender4.2/blender-4.2.16-windows-x64/blender.exe"),
        QStringLiteral("D:/bl4.5/blender-4.2.17-windows-x64/blender.exe"),
        QStringLiteral("D:/bl402/blender-4.0.2-windows-x64/blender-4.0.2-windows-x64/blender.exe"),
        QStringLiteral("D:/bl3600/blender-3.6.0-windows-x64/blender.exe"),
        QStringLiteral("D:/bl36/blender-3.6.2-windows-x64/blender-3.6.2-windows-x64/blender.exe"),
        QStringLiteral("D:/bl350/blender-3.5.0-windows-x64/blender.exe"),
        QStringLiteral("D:/blender332/blender-3.3.2-windows-x64/blender-3.3.2-windows-x64/blender.exe")
    };
    for (const QString& candidate : known) {
        add(candidate);
    }

    return found;
}

QString BlenderBridge::pickDefaultBlenderPath(const QStringList& candidates) const
{
    for (const QString& candidate : candidates) {
        if (candidate.contains(QStringLiteral("5.0.1"), Qt::CaseInsensitive)) {
            return candidate;
        }
    }
    return candidates.isEmpty() ? QString() : candidates.first();
}

QString BlenderBridge::buildMaterialScript(const TextureResult& result, bool liveSession) const
{
    const QString materialName = QStringLiteral("TG_%1").arg(result.jobId);
    const QString base = pythonEscaped(result.filePaths.value(QStringLiteral("base")));
    const QString bump = pythonEscaped(result.filePaths.value(QStringLiteral("bump")));
    const QString normal = pythonEscaped(result.filePaths.value(QStringLiteral("normal")));
    const QString roughness = pythonEscaped(result.filePaths.value(QStringLiteral("roughness")));
    const QString metal = pythonEscaped(result.filePaths.value(QStringLiteral("metal")));
    const QString ao = pythonEscaped(result.filePaths.value(QStringLiteral("ao")));
    const QString alpha = pythonEscaped(result.filePaths.value(QStringLiteral("alpha")));
    const QString outputDirectory = pythonEscaped(result.outputDirectory);
    const bool useAlpha = hasMeaningfulAlpha(result.images.value(QStringLiteral("alpha")));

    return QString::fromUtf8(R"PY(
import bpy
import traceback
from pathlib import Path

live_session = %1
material_name = "%2"
base_path = Path("%3")
bump_path = Path("%4")
normal_path = Path("%5")
roughness_path = Path("%6")
metal_path = Path("%7")
ao_path = Path("%8")
alpha_path = Path("%9")
output_dir = Path("%10")
use_alpha = %11

log_path = output_dir / "blender_import_log.txt"
blend_path = output_dir / "blender_preview.blend"

def log_line(message):
    text = str(message)
    print(text, flush=True)
    try:
        output_dir.mkdir(parents=True, exist_ok=True)
        with log_path.open("a", encoding="utf-8") as handle:
            handle.write(text + "\n")
    except Exception:
        pass

def load_image(path, colorspace="sRGB"):
    if not path.exists():
        log_line(f"Missing map: {path}")
        return None
    image = bpy.data.images.load(str(path), check_existing=True)
    try:
        image.colorspace_settings.name = colorspace
    except Exception as exc:
        log_line(f"Color space warning for {path.name}: {exc}")
    log_line(f"Loaded map: {path.name}")
    return image

def get_active_object():
    # Blender GUI startup contexts do not always expose bpy.context.active_object.
    try:
        return bpy.context.view_layer.objects.active
    except Exception:
        return None

def set_active_object(obj):
    if obj is None:
        return
    try:
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
    except Exception as exc:
        log_line(f"set active warning: {exc}")

def collect_targets():
    # Bridge-style: apply the material to every selected mesh, or the active
    # mesh when nothing is selected. Empty list means "no usable mesh".
    targets = []
    try:
        for candidate in list(bpy.context.view_layer.objects.selected):
            if candidate is not None and candidate.type == "MESH":
                targets.append(candidate)
    except Exception as exc:
        log_line(f"collect selected warning: {exc}")
    if not targets:
        try:
            active = bpy.context.view_layer.objects.active
        except Exception:
            active = None
        if active is not None and active.type == "MESH":
            targets.append(active)
    return targets

def focus_view_on_object(target):
    try:
        screen = bpy.context.screen
        window = bpy.context.window
    except Exception as exc:
        log_line(f"Viewport focus skipped: {exc}")
        return
    if screen is None or window is None:
        log_line("Viewport focus skipped: no window context.")
        return
    for area in screen.areas:
        if area.type != "VIEW_3D":
            continue
        region = next((r for r in area.regions if r.type == "WINDOW"), None)
        space = next((s for s in area.spaces if s.type == "VIEW_3D"), None)
        if region is None or space is None:
            continue
        space.shading.type = "MATERIAL"
        try:
            space.clip_end = 100.0
        except Exception:
            pass
        override = bpy.context.copy()
        override["window"] = window
        override["screen"] = screen
        override["area"] = area
        override["region"] = region
        override["space_data"] = space
        override["active_object"] = target
        override["object"] = target
        override["selected_objects"] = [target]
        override["selected_editable_objects"] = [target]
        try:
            with bpy.context.temp_override(**override):
                bpy.ops.view3d.view_selected(use_all_regions=False)
        except Exception as exc:
            log_line(f"view_selected warning: {exc}")
        return
    log_line("Viewport focus skipped: no 3D view area found.")

log_line(f"Starting Blender import for {material_name}")

try:
    if not live_session:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        log_line("Factory startup scene loaded")

    targets = collect_targets()
    if not targets:
        # No usable selected/active mesh: fall back to a dedicated preview sphere.
        bpy.ops.mesh.primitive_uv_sphere_add(radius=1.0, segments=96, ring_count=64, location=(0.0, 0.0, 0.0))
        obj = get_active_object()
        if obj is None:
            obj = bpy.data.objects.get("Sphere")
        if obj is not None:
            obj.name = "TG_PreviewSphere"
        targets = [obj]
        log_line("No mesh selected; created a preview sphere")

    if not targets or targets[0] is None:
        raise RuntimeError("Could not find an object to apply the material to.")

    log_line(f"Applying the material to {len(targets)} object(s).")

    material = bpy.data.materials.get(material_name)
    if material is None:
        material = bpy.data.materials.new(material_name)
    material.use_nodes = True
    material.blend_method = "HASHED" if use_alpha else "OPAQUE"
    if hasattr(material, "shadow_method"):
        material.shadow_method = "HASHED" if use_alpha else "OPAQUE"

    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    # Bridge-style visual grouping: one colored frame per channel so the node
    # tree reads cleanly in the Shader Editor.
    def make_frame(title, color):
        f = nodes.new("NodeFrame")
        f.label = title
        f.name = title
        f.use_custom_color = True
        f.color = color
        return f

    def tag(node, frame, label):
        node.parent = frame
        if label:
            node.label = label
        return node

    base_frame = make_frame("BASE COLOR", (0.16, 0.42, 0.36))
    ao_frame = make_frame("AO", (0.42, 0.30, 0.16))
    normal_frame = make_frame("NORMAL", (0.32, 0.18, 0.48))
    bump_frame = make_frame("BUMP", (0.40, 0.32, 0.12))
    rough_frame = make_frame("ROUGHNESS", (0.22, 0.46, 0.22))
    metal_frame = make_frame("METALLIC", (0.50, 0.34, 0.16))
    alpha_frame = make_frame("OPACITY", (0.52, 0.22, 0.22))

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (920, 120)
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.location = (560, 120)
    bsdf.inputs["Roughness"].default_value = 0.48
    links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])

    normal_socket = None
    base_color_socket = None

    if base_path.exists():
        tex = nodes.new("ShaderNodeTexImage")
        tex.location = (-760, 260)
        tex.image = load_image(base_path, "sRGB")
        if tex.image is not None:
            tag(tex, base_frame, "Base Color")
            base_color_socket = tex.outputs["Color"]

    if ao_path.exists() and base_color_socket is not None:
        tex = nodes.new("ShaderNodeTexImage")
        tex.location = (-760, 520)
        tex.image = load_image(ao_path, "Non-Color")
        if tex.image is not None:
            tag(tex, ao_frame, "AO")
            multiply = nodes.new("ShaderNodeMixRGB")
            multiply.location = (-360, 340)
            multiply.blend_type = "MULTIPLY"
            multiply.inputs["Fac"].default_value = 1.0
            tag(multiply, ao_frame, "AO Multiply")
            links.new(base_color_socket, multiply.inputs["Color1"])
            links.new(tex.outputs["Color"], multiply.inputs["Color2"])
            base_color_socket = multiply.outputs["Color"]

    if base_color_socket is not None:
        links.new(base_color_socket, bsdf.inputs["Base Color"])

    if normal_path.exists():
        tex = nodes.new("ShaderNodeTexImage")
        tex.location = (-520, 40)
        tex.image = load_image(normal_path, "Non-Color")
        if tex.image is not None:
            tag(tex, normal_frame, "Normal")
            normal_map = nodes.new("ShaderNodeNormalMap")
            normal_map.location = (-180, 40)
            # The relief strength was already baked into the normal map when it
            # was generated, so consume it as-is. Scaling it again here would
            # apply the slider twice (effective strength = slider squared) and
            # would no longer match the app's own 3D preview.
            normal_map.inputs["Strength"].default_value = 1.0
            tag(normal_map, normal_frame, "Normal Map")
            links.new(tex.outputs["Color"], normal_map.inputs["Color"])
            normal_socket = normal_map.outputs["Normal"]

    if bump_path.exists():
        tex = nodes.new("ShaderNodeTexImage")
        tex.location = (-520, -180)
        tex.image = load_image(bump_path, "Non-Color")
        if tex.image is not None:
            tag(tex, bump_frame, "Bump")
            bump = nodes.new("ShaderNodeBump")
            bump.location = (-180, -180)
            # Same reason as the normal map: the relief strength is in bump.png.
            bump.inputs["Strength"].default_value = 1.0
            tag(bump, bump_frame, "Bump")
            links.new(tex.outputs["Color"], bump.inputs["Height"])
            if normal_socket is not None:
                links.new(normal_socket, bump.inputs["Normal"])
            normal_socket = bump.outputs["Normal"]

    if normal_socket is not None:
        links.new(normal_socket, bsdf.inputs["Normal"])

    if roughness_path.exists():
        tex = nodes.new("ShaderNodeTexImage")
        tex.location = (-520, -400)
        tex.image = load_image(roughness_path, "Non-Color")
        if tex.image is not None:
            tag(tex, rough_frame, "Roughness")
            links.new(tex.outputs["Color"], bsdf.inputs["Roughness"])

    if metal_path.exists():
        tex = nodes.new("ShaderNodeTexImage")
        tex.location = (-520, -620)
        tex.image = load_image(metal_path, "Non-Color")
        if tex.image is not None:
            tag(tex, metal_frame, "Metallic")
            links.new(tex.outputs["Color"], bsdf.inputs["Metallic"])

    if use_alpha and alpha_path.exists():
        tex = nodes.new("ShaderNodeTexImage")
        tex.location = (-520, -840)
        tex.image = load_image(alpha_path, "Non-Color")
        if tex.image is not None:
            tag(tex, alpha_frame, "Opacity")
            links.new(tex.outputs["Color"], bsdf.inputs["Alpha"])

    for obj in targets:
        if obj is None:
            continue
        set_active_object(obj)
        try:
            bpy.ops.object.shade_smooth()
        except Exception as exc:
            log_line(f"shade_smooth warning: {exc}")
        if obj.data.materials:
            obj.data.materials[0] = material
        else:
            obj.data.materials.append(material)
        obj.active_material = material

    focus_view_on_object(targets[0])

    if not live_session:
        try:
            bpy.ops.wm.save_as_mainfile(filepath=str(blend_path), copy=True)
            log_line(f"Saved preview file: {blend_path}")
        except Exception as exc:
            log_line(f"save_as_mainfile warning: {exc}")

    result = {
        "message": str(output_dir),
        "material": material.name,
        "object": obj.name,
        "log": str(log_path),
        "blend": str(blend_path),
    }
    log_line("Import finished successfully")
except Exception as exc:
    log_line(f"Import failed: {exc}")
    log_line(traceback.format_exc())
    raise
)PY")
        .arg(liveSession ? QStringLiteral("True") : QStringLiteral("False"))
        .arg(pythonEscaped(materialName))
        .arg(base)
        .arg(bump)
        .arg(normal)
        .arg(roughness)
        .arg(metal)
        .arg(ao)
        .arg(alpha)
        .arg(outputDirectory)
        .arg(useAlpha ? QStringLiteral("True") : QStringLiteral("False"));
}

BlenderBridge::ImportResult BlenderBridge::importViaMcp(const QString& script) const
{
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), 9876);
    if (!socket.waitForConnected(450)) {
        return {false, QStringLiteral("Blender is installed, but no live MCP session is available."), QStringLiteral("offline")};
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("type"), QStringLiteral("execute"));
    payload.insert(QStringLiteral("code"), script);
    payload.insert(QStringLiteral("strict_json"), true);

    QByteArray bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    bytes.append('\0');
    socket.write(bytes);
    if (!socket.waitForBytesWritten(800)) {
        return {false, QStringLiteral("Blender did not accept the request."), QStringLiteral("mcp")};
    }

    // The addon executes the script on Blender's main thread (bpy.app.timers),
    // so building the preview sphere, loading the maps and setting colorspace can
    // take a few seconds, and longer if Blender is mid-operation (Blender only
    // runs timers while its main loop is free). A healthy idle session answers
    // in 2-5s. Poll until the NUL-terminated reply arrives; the addon itself
    // waits at most 45s, so match that ceiling. We do NOT fall back to opening a
    // new Blender on timeout here — that decision lives in importMaterial.
    QByteArray response;
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 45000;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (socket.waitForReadyRead(500)) {
            response.append(socket.readAll());
            if (response.contains('\0')) {
                break;
            }
        } else if (!socket.isOpen()) {
            return {false, QStringLiteral("Blender closed the connection before returning a result."), QStringLiteral("mcp")};
        }
    }
    if (!response.contains('\0')) {
        return {false, QStringLiteral("Blender did not return a result in time."), QStringLiteral("mcp")};
    }

    const int terminatorIndex = response.indexOf('\0');
    if (terminatorIndex >= 0) {
        response = response.left(terminatorIndex);
    }

    const QJsonDocument document = QJsonDocument::fromJson(response);
    if (!document.isObject()) {
        return {false, QStringLiteral("Blender returned an unreadable response."), QStringLiteral("mcp")};
    }

    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("status")).toString() == QStringLiteral("error")) {
        return {false, object.value(QStringLiteral("message")).toString(QStringLiteral("Blender returned an error.")), QStringLiteral("mcp")};
    }

    const QJsonObject result = object.value(QStringLiteral("result")).toObject();
    return {true, result.value(QStringLiteral("message")).toString(QStringLiteral("Material created in Blender.")), QStringLiteral("mcp")};
}

BlenderBridge::ImportResult BlenderBridge::launchBlender(const QString& script, const QString& outputDirectory) const
{
    const QString scriptPath = QDir(outputDirectory).filePath(QStringLiteral("create_material_in_blender.py"));
    QFile file(scriptPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return {false, QStringLiteral("Could not write the Blender import script."), QStringLiteral("launch")};
    }
    file.write(script.toUtf8());
    file.close();

    const bool started = QProcess::startDetached(m_blenderPath,
                                                 {QStringLiteral("--factory-startup"), QStringLiteral("--python"), scriptPath});
    if (!started) {
        return {false, QStringLiteral("Blender could not be launched."), QStringLiteral("launch")};
    }

    return {true, QDir::toNativeSeparators(outputDirectory), QStringLiteral("launch")};
}
