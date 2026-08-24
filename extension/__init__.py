"""TG Blender Link.

Receives generated texture maps from the Texture Generator desktop app over a
localhost socket and builds a preview material in the *current* Blender
project, so the app can import into a running session instead of launching a
new instance.

Delivery is fire-and-forget: the app does not wait for Blender to finish. The
socket thread acknowledges the request immediately and the generated script
runs later on Blender's main thread, whenever it is free. Blender only runs
its timers while the main loop is idle (viewport drags, modal operators and
long Python scripts stall them), so a synchronous round trip would let the
app time out every time Blender is busy.

User-visible strings are English by default (platform review requirement);
Simplified Chinese is provided through Blender's translation system and
follows the UI language. See _TRANSLATIONS below.
"""

import bpy
import json
import os
import socket
import tempfile
import threading
import traceback
from collections import deque

from bpy.app.translations import pgettext_rpt as rpt_
from bpy.app.translations import pgettext_iface as ui_

OP_IDNAME = "bl_ext.tg_link_test"

HOST = "127.0.0.1"
PORT = 9876

_pending = deque(maxlen=16)       # queued code strings, drained on the main thread
_stop = threading.Event()         # set to stop the accept loop
_server = None
_server_thread = None
_start_error = ""                 # English fallback (console / safety)
_start_error_exc = ""             # exc text, for building a localized message

# ---------------------------------------------------------------------------
# User-visible text (English default). Simplified Chinese follows Blender's UI
# language. Blender's translation lookup does not fall back to "*" for explicit
# contexts, so every string is registered under "*" AND its specific context.
# ---------------------------------------------------------------------------
_TRANSLATIONS = {
    # --- operator name / description ---
    ("Operator", "Test TG Connection"): "测试 TG 连接",
    ("operator_default", "Test TG Connection"): "测试 TG 连接",
    ("*", "Test TG Connection"): "测试 TG 连接",
    ("OperatorTip", "Check whether the Texture Generator link server is running"): "检查贴图生成器连接服务是否在运行",
    ("Tip", "Check whether the Texture Generator link server is running"): "检查贴图生成器连接服务是否在运行",
    ("*", "Check whether the Texture Generator link server is running"): "检查贴图生成器连接服务是否在运行",
    # --- panel ---
    ("*", "TG Texture Receiver"): "TG 贴图接收",
    ("*", "Listening on {}:{}"): "监听 {}:{}",
    ("*", "Texture Generator can send to this project"): "贴图生成器可发送到当前工程",
    ("*", "Pending: {} request(s)"): "待处理 {} 个请求",
    ("*", "Not listening"): "未监听",
    ("*", "Server not running."): "服务未运行。",
    ("*", "Port {} is already in use ({}). Another Blender instance may have this extension enabled."): "端口 {} 已被占用（{}）。可能有另一个 Blender 实例已启用本扩展。",
    # --- report messages ---
    ("*", "TG Blender Link listening on {}:{}."): "TG Blender Link 正在监听 {}:{}。",
    ("Report", "TG Blender Link listening on {}:{}."): "TG Blender Link 正在监听 {}:{}。",
    ("*", "TG Blender Link server is not running."): "TG Blender Link 服务未运行。",
    ("Report", "TG Blender Link server is not running."): "TG Blender Link 服务未运行。",
}

_translations_dict = {
    "zh_HANS": dict(_TRANSLATIONS),
    "zh_CN": dict(_TRANSLATIONS),
}


def _error_log(exc):
    """Write an exec-level failure somewhere discoverable. The generated script
    writes its own rich log (blender_import_log.txt) once it starts running."""
    try:
        path = os.path.join(tempfile.gettempdir(), "tg_blender_link_error.log")
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(traceback.format_exc())
            handle.write("\n")
        print(f"[TG Blender Link] exec error ({path}): {exc}")
    except Exception:
        print(f"[TG Blender Link] exec error: {exc}")


def _port_in_use_message():
    """Localized version of the startup error, safe to call on the main thread."""
    if not _start_error_exc:
        return _start_error
    return rpt_(
        "Port {} is already in use ({}). Another Blender instance may have this extension enabled."
    ).format(HOST, _start_error_exc)


def _handle_connection(conn):
    data = b""
    conn.settimeout(3.0)
    try:
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            data += chunk
            if b"\x00" in chunk:
                break
    except (socket.timeout, OSError):
        pass

    payload = {}
    if data:
        try:
            payload = json.loads(data.split(b"\x00")[0].decode("utf-8"))
        except Exception:
            payload = {}

    if payload.get("type") == "execute" and isinstance(payload.get("code"), str):
        _pending.append(payload["code"])
        response = {"status": "ok", "result": {"message": "accepted"}}
    else:
        response = {"status": "error", "message": "Unsupported request type."}

    try:
        conn.sendall(json.dumps(response).encode("utf-8") + b"\x00")
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def _server_loop():
    global _server, _start_error, _start_error_exc
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind((HOST, PORT))
    except OSError as exc:
        _start_error = (
            f"Port {PORT} is already in use ({exc}). "
            "Another Blender instance may have this extension enabled."
        )
        _start_error_exc = str(exc)
        print(f"[TG Blender Link] {_start_error}")
        try:
            srv.close()
        except OSError:
            pass
        return
    srv.listen(4)
    srv.settimeout(0.5)
    _server = srv
    while not _stop.is_set():
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        threading.Thread(target=_handle_connection, args=(conn,), daemon=True).start()
    try:
        srv.close()
    except OSError:
        pass


def _timer_callback():
    """Run on the Blender main thread: execute queued requests one at a time."""
    if not _pending:
        return 0.05
    code = _pending.popleft()
    try:
        # The generated script imports what it needs itself; a fresh builtins
        # namespace is auto-injected by exec. Provide bpy plus a few commonly
        # used modules so the script does not NameError.
        exec(compile(code, "<tg_blender_link>", "exec"), {"bpy": bpy})
    except Exception as exc:
        _error_log(exc)
    return 0.05


class TG_OT_blender_link_test(bpy.types.Operator):
    bl_idname = OP_IDNAME
    bl_label = "Test TG Connection"
    bl_description = "Check whether the Texture Generator link server is running"

    def execute(self, context):
        if _server is not None:
            self.report({"INFO"}, rpt_("TG Blender Link listening on {}:{}.").format(HOST, PORT))
        else:
            message = _port_in_use_message() or rpt_("TG Blender Link server is not running.")
            self.report({"ERROR"}, message)
        return {"FINISHED"}


class TG_PT_blender_link(bpy.types.Panel):
    bl_label = "TG Texture Receiver"
    bl_idname = "TG_PT_blender_link"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "TG"

    def draw(self, context):
        layout = self.layout
        if _server is not None:
            box = layout.box()
            box.label(text=ui_("Listening on {}:{}").format(HOST, PORT), icon="CHECKMARK")
            box.label(text=ui_("Texture Generator can send to this project"), icon="MATERIAL")
            if _pending:
                box.label(text=ui_("Pending: {} request(s)").format(len(_pending)), icon="TIME")
        else:
            box = layout.box()
            box.label(text=ui_("Not listening"), icon="ERROR")
            message = _port_in_use_message() or ui_("Server not running.")
            box.label(text=message, icon="INFO")
        layout.operator(OP_IDNAME)


def register():
    global _server_thread, _start_error, _start_error_exc
    _stop.clear()
    _start_error = ""
    _start_error_exc = ""
    bpy.app.translations.register(__name__, _translations_dict)
    _server_thread = threading.Thread(target=_server_loop, daemon=True)
    _server_thread.start()
    bpy.app.timers.register(_timer_callback)
    bpy.utils.register_class(TG_OT_blender_link_test)
    bpy.utils.register_class(TG_PT_blender_link)


def unregister():
    bpy.utils.unregister_class(TG_PT_blender_link)
    bpy.utils.unregister_class(TG_OT_blender_link_test)
    _stop.set()
    try:
        bpy.app.timers.unregister(_timer_callback)
    except Exception:
        pass
    bpy.app.translations.unregister(__name__)
