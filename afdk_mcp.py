#!/usr/bin/env python3
"""MCP server for AFDK Remastered (stdio, no dependencies besides Python 3.8+ on Windows).

It talks to _afdk_remastered_mcp.dll inside a running AFDK.exe through the named pipe \\\\.\\pipe\\afdk_remastered.
Put this file next to AFDK.exe (with _afdk_remastered.dll and _afdk_remastered_mcp.dll) and register it in your agent:
    python C:\\path\\to\\AFDK\\afdk_mcp.py
"""
import base64
import json
import os
import struct
import subprocess
import sys
import time
import zlib

PIPE = r"\\.\pipe\afdk_remastered"
HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "AFDK.exe")
PROTOCOL = "2025-06-18"
__version__ = "1.0"                               # build.ps1 puts the release version here

INSTRUCTIONS = """Controls AFDK Remastered, a logic-circuit editor and simulator (Windows).
Coordinates are scheme cells (integers), not pixels. Call afdk_state first: it lists elements (with their input and
output pin cells), nodes (named variables) and wires. Connect things by drawing wires between pin cells; every wire
segment must be horizontal or vertical. Nodes are the circuit's inputs/outputs: put a node on a pin cell or a wire end.
Editing works only in editor mode; use afdk_mode to switch to model (simulation) mode and back.
Take an afdk_screenshot to check the result visually. If a dialog of the program blocks work, afdk_windows shows it
and afdk_press_button answers it."""


class AfdkError(Exception):
    pass


_pipe = None


def _connect(wait=0.0):
    global _pipe
    deadline = time.time() + wait
    while True:
        try:
            _pipe = open(PIPE, "r+b", buffering=0)
            return
        except OSError:
            if time.time() >= deadline:
                raise AfdkError("AFDK is not running (or was started without _afdk_remastered_mcp.dll). "
                                "Use afdk_launch to start it.")
            time.sleep(0.3)


def call(req):
    """send one request line to the DLL, return the decoded JSON answer"""
    global _pipe
    for attempt in (0, 1):
        try:
            if _pipe is None:
                _connect(wait=5 if attempt else 0)
            _pipe.write(req.encode("utf-8"))
            data = _pipe.read(1 << 26)
            if not data:
                raise OSError("pipe closed")
            ans = json.loads(data.decode("utf-8"))
            if not ans.get("ok"):
                raise AfdkError(ans.get("error", "failed"))
            return ans
        except OSError:
            try:
                if _pipe:
                    _pipe.close()
            except OSError:
                pass
            _pipe = None
            if attempt:
                raise AfdkError("lost the connection to AFDK")


def q(s):
    """quote one argument for the DLL's command line"""
    s = str(s)
    if not s or any(c in s for c in ' \t"\\'):
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return s


# ---------- BMP (24-bit, bottom-up) -> PNG ----------
def bmp_to_png(bmp):
    off = struct.unpack_from("<I", bmp, 10)[0]
    w, h = struct.unpack_from("<ii", bmp, 18)
    bpp = struct.unpack_from("<H", bmp, 28)[0]
    if bpp != 24:
        raise AfdkError("unexpected bitmap format")
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(abs(h)):
        src = h - 1 - y if h > 0 else y
        row = bmp[off + src * stride: off + src * stride + w * 3]
        rgb = bytearray(row)
        rgb[0::3], rgb[2::3] = row[2::3], row[0::3]
        rows.append(b"\x00" + bytes(rgb))
    raw = zlib.compress(b"".join(rows), 6)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, abs(h), 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", raw) + chunk(b"IEND", b""))


# ---------- tools ----------
def pt(name):
    return {"type": "integer", "description": name}


TOOLS = [
    ("afdk_launch", "Start AFDK (if it is not running) and wait until it answers. Optionally open a project file.",
     {"project": {"type": "string", "description": "path to a .AFP project to open (optional)"}}, []),
    ("afdk_state", "The open project: path, modified flag, mode, zoom, and every element (label, cell, size, input and "
     "output pin cells), node (name, cell) and wire (end cells).", {}, []),
    ("afdk_library", "All elements of the library, as paths like 'Стандартные/И/2И'.", {}, []),
    ("afdk_add_element", "Place a library element with its top-left corner at cell (x, y). Returns its id.",
     {"item": {"type": "string", "description": "library path from afdk_library, e.g. 'Стандартные/И/2И'"},
      "x": pt("cell x"), "y": pt("cell y")}, ["item", "x", "y"]),
    ("afdk_add_wire", "Draw a wire through the given cells (a polyline). Each segment must be horizontal or vertical. "
     "Wires connect where an end touches a pin, a node or another wire.",
     {"points": {"type": "array", "minItems": 2, "items": {"type": "array", "items": {"type": "integer"},
                                                           "minItems": 2, "maxItems": 2},
                 "description": "[[x1,y1],[x2,y2],...]"}}, ["points"]),
    ("afdk_add_node", "Create a named node (circuit variable, input or output) at cell (x, y).",
     {"name": {"type": "string"}, "x": pt("cell x"), "y": pt("cell y")}, ["name", "x", "y"]),
    ("afdk_select", "Select everything inside the rectangle of cells (rubber band). Clears the old selection.",
     {"x1": pt("corner x"), "y1": pt("corner y"), "x2": pt("corner x"), "y2": pt("corner y")},
     ["x1", "y1", "x2", "y2"]),
    ("afdk_select_all", "Select everything.", {}, []),
    ("afdk_deselect", "Clear the selection.", {}, []),
    ("afdk_delete_selection", "Delete the selected items.", {}, []),
    ("afdk_move_selection", "Move the selected items by (dx, dy) cells; attached wires stretch as in the program.",
     {"dx": pt("cells"), "dy": pt("cells")}, ["dx", "dy"]),
    ("afdk_duplicate_selection", "Copy the selected items one cell down-right (like Ctrl+D).", {}, []),
    ("afdk_undo", "Undo the last change.", {}, []),
    ("afdk_redo", "Redo.", {}, []),
    ("afdk_open", "Open a project file (.AFP).", {"path": {"type": "string"}}, ["path"]),
    ("afdk_new", "Create a new empty project.", {}, []),
    ("afdk_save", "Save the project (File > Save).", {}, []),
    ("afdk_save_as", "Save the project under a new path.", {"path": {"type": "string"}}, ["path"]),
    ("afdk_close", "Close the active project (the program may ask to save: see afdk_windows).", {}, []),
    ("afdk_mode", "Switch between editing and simulation.",
     {"mode": {"type": "string", "enum": ["editor", "model"]}}, ["mode"]),
    ("afdk_click", "Click the scheme at cell (x, y) with the current tool; in model mode this is how inputs are "
     "toggled.", {"x": pt("cell x"), "y": pt("cell y"), "right": {"type": "boolean"}}, ["x", "y"]),
    ("afdk_zoom", "Set the zoom level (5..40, the program's default is 10).", {"level": pt("5..40")}, ["level"]),
    ("afdk_menu_command", "Run a menu command by id. File: 2 New, 3 Open, 4 Save, 5 Save as, 6 Close, 8 Exit. "
     "Edit: 10 Cut, 11 Copy, 12 Paste, 13 Delete. Project: 15 Chip package, 16 Package editor, 17 Compile, "
     "18 Add to library, 20 Settings. Modeling (model mode): 22 Step, 23 Run interval, 24 Run until, 25 Generator. "
     "Tools: 27 Library editor, 28 Settings, 30 Timing diagram, 31 Table. Window: 33-37.",
     {"id": pt("menu command id")}, ["id"]),
    ("afdk_screenshot", "Picture of the program: 'window' (main window), 'canvas' (scheme only), 'active' (the window "
     "in front, e.g. a dialog), or a window class such as 'TDiagram' (timing diagram) or 'TTable'.",
     {"target": {"type": "string", "default": "canvas"}}, []),
    ("afdk_windows", "Visible windows and dialogs of the program with their controls' texts.", {}, []),
    ("afdk_press_button", "Press a button in an open dialog by its caption (e.g. 'OK', 'Отмена', 'Yes', 'No').",
     {"text": {"type": "string"}}, ["text"]),
]


def launch(project=None):
    try:
        call("ping")
        running = True
    except AfdkError:
        running = False
    if not running:
        if not os.path.exists(EXE):
            raise AfdkError("AFDK.exe not found next to afdk_mcp.py (%s)" % EXE)
        args = [EXE] + ([os.path.abspath(project)] if project else [])
        subprocess.Popen(args, cwd=HERE)
        _connect(wait=30)
        call("ping")
        return "started" + (" with " + project if project else "")
    if project:
        call("open " + q(os.path.abspath(project)))
        return "already running; opening " + project
    return "already running"


def run_tool(name, a):
    """returns MCP content items"""
    def text(obj):
        return [{"type": "text", "text": obj if isinstance(obj, str) else json.dumps(obj, ensure_ascii=False)}]

    def strip(ans):
        ans = dict(ans)
        ans.pop("ok", None)
        return ans or "done"

    if name == "afdk_launch":
        return text(launch(a.get("project")))
    if name == "afdk_state":
        return text(strip(call("state")))
    if name == "afdk_library":
        return text(call("library")["items"])
    if name == "afdk_add_element":
        return text(strip(call("add %s %d %d" % (q(a["item"]), a["x"], a["y"]))))
    if name == "afdk_add_wire":
        pts = a["points"]
        if len(pts) < 2:
            raise AfdkError("at least two points")
        return text(strip(call("wire " + " ".join("%d %d" % (p[0], p[1]) for p in pts))))
    if name == "afdk_add_node":
        return text(strip(call("node %s %d %d" % (q(a["name"]), a["x"], a["y"]))))
    if name == "afdk_select":
        return text(strip(call("select %d %d %d %d" % (a["x1"], a["y1"], a["x2"], a["y2"]))))
    simple = {"afdk_select_all": "select_all", "afdk_deselect": "deselect", "afdk_delete_selection": "delete",
              "afdk_duplicate_selection": "duplicate", "afdk_undo": "undo", "afdk_redo": "redo", "afdk_new": "new",
              "afdk_save": "save", "afdk_close": "close"}
    if name in simple:
        return text(strip(call(simple[name])))
    if name == "afdk_move_selection":
        return text(strip(call("move %d %d" % (a["dx"], a["dy"]))))
    if name in ("afdk_open", "afdk_save_as"):
        cmd = "open" if name == "afdk_open" else "save_as"
        call("%s %s" % (cmd, q(os.path.abspath(a["path"]))))
        time.sleep(1.0)
        return text(strip(call("state")).get("project") or "no project")
    if name == "afdk_mode":
        return text(strip(call("mode " + a["mode"])))
    if name == "afdk_click":
        return text(strip(call("click %d %d%s" % (a["x"], a["y"], " right" if a.get("right") else ""))))
    if name == "afdk_zoom":
        return text(strip(call("zoom %d" % a["level"])))
    if name == "afdk_menu_command":
        return text(strip(call("command %d" % a["id"])))
    if name == "afdk_screenshot":
        ans = call("screenshot " + q(a.get("target") or "canvas"))
        png = bmp_to_png(base64.b64decode(ans["bmp"]))
        return [{"type": "image", "data": base64.b64encode(png).decode("ascii"), "mimeType": "image/png"}]
    if name == "afdk_windows":
        return text(call("windows")["windows"])
    if name == "afdk_press_button":
        return text(strip(call("press " + q(a["text"]))))
    raise AfdkError("unknown tool " + name)


# ---------- JSON-RPC over stdio ----------
def reply(id_, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": id_}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    sys.stdout.write(json.dumps(msg, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def handle(msg):
    method, id_ = msg.get("method"), msg.get("id")
    params = msg.get("params") or {}
    if id_ is None:                                   # notification
        return
    if method == "initialize":
        reply(id_, {"protocolVersion": params.get("protocolVersion") or PROTOCOL,
                    "capabilities": {"tools": {"listChanged": False}},
                    "serverInfo": {"name": "afdk-remastered", "version": __version__},
                    "instructions": INSTRUCTIONS})
    elif method == "ping":
        reply(id_, {})
    elif method == "tools/list":
        reply(id_, {"tools": [{"name": n, "description": d,
                               "inputSchema": {"type": "object", "properties": props, "required": req}}
                              for n, d, props, req in TOOLS]})
    elif method == "tools/call":
        try:
            content = run_tool(params.get("name"), params.get("arguments") or {})
            reply(id_, {"content": content, "isError": False})
        except (AfdkError, KeyError, ValueError, TypeError) as e:
            reply(id_, {"content": [{"type": "text", "text": "Error: %s" % e}], "isError": True})
    elif method in ("resources/list", "prompts/list"):
        reply(id_, {method.split("/")[0]: []})
    else:
        reply(id_, error={"code": -32601, "message": "method not found: %s" % method})


def main():
    if hasattr(sys.stdin, "reconfigure"):
        sys.stdin.reconfigure(encoding="utf-8")
        sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            reply(None, error={"code": -32700, "message": "parse error"})
            continue
        for m in (msg if isinstance(msg, list) else [msg]):
            handle(m)


if __name__ == "__main__":
    main()
