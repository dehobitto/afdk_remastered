import ctypes, sys
from ctypes import wintypes as w
u = ctypes.windll.user32
u.SetProcessDPIAware()
pid = int(sys.argv[1])

def info(h):
    c = ctypes.create_unicode_buffer(128); u.GetClassNameW(h, c, 128)
    t = ctypes.create_unicode_buffer(128); u.GetWindowTextW(h, t, 128)
    r = w.RECT(); u.GetWindowRect(h, ctypes.byref(r))
    return f"{h:#x} {c.value!r} {t.value!r} ({r.left},{r.top},{r.right},{r.bottom}) vis={bool(u.IsWindowVisible(h))}"

def walk(h, d=0):
    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def cb(ch, l):
        p = w.DWORD(); u.GetWindowThreadProcessId(ch, ctypes.byref(p))
        print("  " * d + info(ch)); walk(ch, d + 1); return True
    u.EnumChildWindows(h, cb, 0) if d == 0 else None
    if d:
        u.EnumChildWindows(h, cb, 0)

@ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
def top(h, l):
    p = w.DWORD(); u.GetWindowThreadProcessId(h, ctypes.byref(p))
    if p.value == pid:
        print(info(h)); walk(h, 1)
    return True
u.EnumWindows(top, 0)
