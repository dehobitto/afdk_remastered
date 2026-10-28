# AFDK Remastered — notes for agents

AFDK 2.0 is a Delphi 5 logic-circuit editor with no source. We patch it by injecting `_afdk_remastered.dll` (C, 32-bit): the exe gets an extra import so the DLL loads first, and the DLL hooks the program from there. The user is Russian/Ukrainian-speaking. User-facing docs and `.ini` comments are in Ukrainian; code comments are in English.

## Layout

```
C:\AFDKdev\                 installed copy the user runs (exe/DLLs there are build output, not in git): AFDK.exe, _afdk_remastered.dll, _afdk_remastered.ini, Lib\, Projects\
  README.md                 user docs (Ukrainian): features, hotkeys, ini keys
  patch\build.ps1           builds dist\ (DLL + injected exe), copies Lib/Projects into dist on first build
  patch\inject_import.py    adds the DLL import to the original exe
  patch\orig\AFDK.exe       untouched original (sha256 5ab550ce...) - never modify; the build input
  patch\dist\               build output + its own Lib/Projects for testing
  patch\re\all.c            full Ghidra decompile (FUN_xxxxxxxx names = addresses), not in git. grep it instead of re-analysing
  patch\src\                DLL sources, single translation unit:
    afdkfix.c   DllMain, registry redirect (HKCU\Software\DCA Laboratory\...), Unicode file dialogs, fx_log, VEH log
    common.h    F32/U8 field access, dcall(), shared addresses
    locale.c    Cyrillic under a non-1251 Windows code page (see below)
    feat.c      includes cfg/gfx/feat2-4/filekey; WH_GETMESSAGE hook getmsg_proc: wheel zoom, pan, hotkeys, dbl-click node
    cfg.c       _afdk_remastered.ini (default text in Ukrainian, written on first start)
    gfx.c       thicker pens on canvas DCs, font scaling + Cyrillic charset, visual styles (actctx), title, CreateWindowEx/SetWindowText/MessageBox A->W
    feat2.c     undo/redo (whole-scheme snapshots via save/load stream), ghost paste under cursor
    feat3.c     wire tip dragging with an elbow, right-click wire delete
    feat4.c     select all, duplicate, new node (V), autosave + .bak, per-project zoom/scroll memory, 1 s tick_proc
    filekey.c   File > Open key fix (old Lab1.AFP opened with "Read beyond end of file")
    mcp_api.h   AFDK_API struct the main DLL passes to the MCP DLL (McpStart); feat4.c mcp_load() loads it on the first tick
    mcp.c       _afdk_remastered_mcp.dll: named pipe \\.\pipe\afdk_remastered (owner-only DACL, no remote clients),
                text commands -> JSON, executed on the UI thread through a hidden HWND_MESSAGE window
    afdk_mcp.py stdio MCP server (stdlib only) that maps MCP tools to the pipe commands; build copies it to dist
    wires.c     "wires follow moved element" DLL - NOT built and NOT shipped (user rejected it). Don't revive it.
```

## Build / install / test

- Build: `powershell -File C:\AFDKdev\patch\build.ps1` (clang from llvm-mingw, i686). Kill AFDK.exe first, because the DLL is locked while it runs.
- Install: copy `patch\dist\AFDK.exe`, `_afdk_remastered.dll`, `_afdk_remastered_mcp.dll` and `afdk_mcp.py` to `C:\AFDKdev\`. Do not overwrite the user's `C:\AFDKdev\_afdk_remastered.ini`, which holds their Title.
- Run a test: `python patch\test\open2.py lab2_1.AFP`. It kills AFDK, launches `dist\AFDK.exe` with the project on the command line and focuses it. `patch\test\cmd.py <id>` posts WM_COMMAND to the main form (4 = Save).
- UI automation helpers (click, drag, key, screenshot): `scratchpad\tools\ui.py` from earlier sessions. They may be gone; if so, rewrite them with SendInput plus PowerShell CopyFromScreen. Prefer the MCP pipe over mouse automation.
- Before UI tests, delete `HKCU\Software\DCA Laboratory\AFDKfix`. The remembered zoom and scroll otherwise shift coordinates.
- Error log (always on): `_afdk_remastered.log` next to the exe gets every Delphi exception the program shows (class, message, raise address, EBP call chain) and every failed CreateWindowEx. Ask the user for it when they report an error box.
- Driving the program without the mouse: with `_afdk_remastered_mcp.dll` present, open `//./pipe/afdk_remastered` in Python (`open(..., 'r+b', buffering=0)`), write one command, read one JSON answer. Commands: `ping state state_raw dump <hex> <n> library add "<lib path>" x y  wire x1 y1 x2 y2 ... node "<name>" x y  select x1 y1 x2 y2  select_all deselect delete duplicate move dx dy  undo redo  mode editor|model  click x y [right]  zoom n  command <menu id>  new save close open "<path>" save_as "<path>"  screenshot canvas|window|active|<class>  windows press "<button>"`. `state_raw` adds object addresses and `dump` reads memory, which is useful for reverse-engineering fields at runtime.
- Debug log: set the env var `AFDKFIX_LOG=<path>` before launch. `fx_log()` writes there, and the VEH logs exceptions, including Delphi 0x0EEDFADE ones with the object name.
- Typing Cyrillic in tests: post WM_INPUTLANGCHANGEREQUEST with the 00000422 layout, then send real VKs. VK_PACKET (KEYEVENTF_UNICODE) under the US layout gives junk, and that is expected.

## Calling the program

- `dcall(fn, eax, edx, ecx, nstack, s1, s2)` uses Delphi register convention. **Ghidra lists stack params in reverse**: Ghidra's param_4 is s2 and param_5 is s1.
- **x87 hazard:** Delphi code does not preserve FPU registers across dcall. Convert doubles to ints before any dcall (this caused the zoom-to-corner bug).
- Key objects: main form `**(0x4e4cac)` / `**(0x4e4b20)`. Canvas fields: `+0x208/+0x20c` scroll bars, `+0x22c` zoom level byte (5..40), `+0x224/0x228` extents. VCL `FText` is at offset 0x54.
- Useful functions: F_SET_ZOOM 0x47d67c, F_PASTE 0x4d5be8, F_COPY 0x4d5a84, F_SAVE_STREAM 0x4893f4, F_LOAD_STREAM 0x489388, F_REBUILD 0x4d22d4, F_WIRE_SETGEOM 0x4d0c98, F_TOOL_SET 0x474c1c, mouse down/drag/up 0x4d1cf0/0x4d29f0/0x4d1e1c. More are in the `#define`s at the top of each feat*.c.
- Scheme data (scheme = `*(doc+0x34)`, doc = `*(pf+0x314)`):
  - Lists: elements `+0x10`, junctions `+0x14`, wires `+0x18`; items come from `LI(list, i)`, 1-based, with the count at `+0`.
  - Element: `+0x44/+0x48` cell, `+0x50` selected, `+8` definition.
  - Definition: `+0x21c/+0x220` size, `+0x224` label ShortString ("&", "1"), `+0x214/+0x218` input/output pin arrays (Delphi dynamic arrays, length at -4). Pins are at (x-1, y+k) and (x+w+1, y+k).
  - Wire: `+0x24/+0x28` start, `+0x2c/+0x30` end offset, `+0x18` selected, `+0x14` -> {flag byte, ShortString name}. A named zero-length wire is a node.
- Menu ids: File 2 New, 3 Open, 4 Save, 5 Save as, 6 Close, 8 Exit; Edit 10-13 Cut/Copy/Paste/Delete; Project 15-18, 20 Settings; Modeling 22 Step, 23 Run interval, 24 Run until, 25 Generator; Tools 27 Library editor, 28 Settings, 30 Diagram, 31 Table. A WM_COMMAND for an item still disabled is ignored. The VCL enables items in its idle update, so call the underlying function instead (e.g. Delete -> F_DELETE_SEL 0x4d1bcc).
- Mode switch: TMainForm.AModelExecute 0x4d9b24 / AEditExecute 0x4db02c, called as `dcall(fn, main_form(), main_form())`.
- `F_MOUSE_DOWN` 0x4d1cf0 is the selection-mode handler only. Tools (line, node, placing a library element) live in the canvas window's mouse handlers. To act like a user, send WM_MOUSEMOVE/WM_LBUTTONDOWN/UP to the TFrpaint window at view = cell*zoom - scrollpos (see mcp.c click/drag). A line is one drag per segment. Placing an element = select it in the library tree, then click.
- Undo restore: before LoadStream, unregister the old project lib object from the container and tear the model down with `0x4d5024(doc+8)`. Otherwise saving after undo + paste writes a file that crashes at 004DF830.

## Hooks map

- IAT hooks (`iat_hook`; Borland imports have no OriginalFirstThunk, so they match by resolved address): registry, file dialogs, CreateFile/FindFirst, menu A functions, DefWindowProcA/DefMDIChildProcA/DefFrameProcA, GetWindowTextA, CreateWindowExA, SetWindowTextA, MessageBoxA, CreateFontIndirectA, GDI line/shape calls, GetProcAddress (styles), GetClassInfoA (COMBOBOX kept classic).
- Hotpatches in KernelBase: MultiByteToWideChar, WideCharToMultiByte, GetCPInfo, GetACP make CP_ACP become 1251. NLS tables are reset via RtlInitNlsTables + RtlResetRtlTranslations with c_1251.nls from `%windir%\Sysnative`.
- WH_GETMESSAGE hook `getmsg_proc` (feat.c) handles input. A WinEvent hook fixes captions VCL wrote as ANSI. `tick_proc` is a 1 s timer.
- Code patches in the exe image: filekey.c redirects the call at 0x4dadfb to a stub that saves and restores the global file key `*(*(0x4e4db8))+0x10`, and makes the machine key function 0x47f0a0 return a constant.

## Pitfalls

- Source files are **CRLF**. Python edits must use `newline=''` and replace `\r\n`-aware strings, or they silently do nothing. Prefer the Edit tool.
- Writing C escapes through shell heredocs can halve backslashes. The code uses `NL` ("\015\012") and numeric char codes.
- Do not run headless Ghidra or `re\asm.sh`: they trigger a firewall prompt. Use `re\all.c` or the REA MCP tools on `patch\orig\AFDK.exe`.
- Don't scale "MS Sans Serif" or "Arial" fonts: Delphi dialogs lose fields.
- Comctl v6 COMBOBOX / LISTBOX / SCROLLBAR fail when the VCL recreates them (Win32 error 1447, "does not have scroll bars"). Keep them classic through the GetClassInfoA exclusion.
- Captions: any window created through the CreateWindowExW path (a name with high bytes) needs caption_fix afterwards, child windows too. The ANSI window procedure stores a '?'-converted copy. DefWindowProcA WM_GETTEXT is converted for all windows.
- Escapes keep breaking in shell heredocs: `\a` becomes a bell character (0x07), and `\\.\pipe` gets halved. For text containing backslashes, use the Edit/Write tools or chr(92) in Python.
- Screens change, so take a screenshot first. The user switched from 1080x1920 portrait to 2560x1440. ui.py calls SetProcessDPIAware.
- UI tests touch the user's install when they run `C:\AFDKdev\AFDK.exe`. Back up `Projects\*.AFP` first, because autosave writes them.
- In the element-parameters dialog Esc does nothing; tests must click OK or Отмена.
- Undo is off in Model (simulation) mode by design.
- Wires-follow: the user explicitly does not want it back.
