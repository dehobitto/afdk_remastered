# AFDK Remastered

A patch for AFDK 2.0. Download the zip from [Releases](../../releases/latest) and unpack it next to `Lib` and `Projects` of your AFDK.

## Features

| Feature | How |
|---|---|
| Zoom | mouse wheel, toward the cursor |
| Pan | hold the middle button and drag |
| Undo / redo | `Ctrl+Z` / `Ctrl+Y`, 100 steps, including edits made in dialogs |
| Copy / paste | `Ctrl+C` / `Ctrl+V`; the block previews under the cursor, click to place, `Esc` or right-click to cancel |
| Select all | `Ctrl+A` |
| Duplicate | `Ctrl+D` |
| New node | `V` or double-click empty space |
| Delete a wire | right-click it |
| Drag a wire end | left-drag a free end or an end on a pin; dragging sideways makes a bend |
| Nudge selection | arrow keys (1 cell), `Shift`+arrows (5 cells) |
| Autosave | every 2 minutes; `<project>.AFP.bak` keeps the version from before the session |
| View memory | zoom and scroll are remembered per project |
| Look | thicker lines, larger tree font, modern Windows controls |
| Cyrillic | correct on English Windows, no locale change needed |
| AI agents | MCP server (below) |

## MCP

Requires Python 3.8+ in `PATH`. `<afdk>` is the folder with `AFDK.exe`.

**Claude Code**
```
claude mcp add afdk --scope user -- python <afdk>\afdk_mcp.py
```

**Codex**
```
codex mcp add afdk -- python <afdk>\afdk_mcp.py
```

**opencode**: add to `opencode.json`:
```json
"mcp": { "afdk": { "type": "local", "command": ["python", "<afdk>\\afdk_mcp.py"] } }
```

**pi**: `pi install npm:pi-mcp-adapter`, then add to `~/.config/mcp/mcp.json`:
```json
{ "mcpServers": { "afdk": { "command": "python", "args": ["<afdk>\\afdk_mcp.py"] } } }
```

**Claude Desktop**: same JSON as for pi, in `%APPDATA%\Claude\claude_desktop_config.json`.

The agent can launch AFDK, open and save projects, place elements, draw wires, add nodes, select, move, delete, undo, switch to simulation and take screenshots. It does not use your mouse.

## Settings: `_afdk_remastered.ini`

Created next to `AFDK.exe` on first start. Changes apply after a restart.

| Key | Default | Meaning |
|---|---|---|
| `[View] LineWidth` | 2 | line thickness in pixels; 1 = original |
| `[View] FontScale` | 125 | tree and menu font size, % |
| `[View] Font` | Segoe UI | library tree font |
| `[View] WindowsStyle` | 1 | 1 = modern controls, 0 = original |
| `[Edit] HitRadius` | 8 | how close (px) a click grabs a wire or its end |
| `[Edit] AutosaveSeconds` | 120 | autosave period; 0 = off |
| `[System] CodePage` | 1251 | text code page; 0 = Windows setting |

## Limitations

- No undo in Model mode.
- Dialog fonts are not enlarged: the dialogs lose fields at other sizes.
- Toolbar icons keep their old size.
- `Esc` does not close the element parameters dialog (as in the original); use Cancel.
- Type Cyrillic with a Ukrainian or Russian layout; the on-screen keyboard in the English layout gives wrong characters.
- New files with Cyrillic names get garbled names on English Windows; use Latin file names.
