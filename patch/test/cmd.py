import ctypes, sys, time
from ctypes import wintypes as w
u = ctypes.windll.user32
def main():
    r = []
    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def cb(h, l):
        b = ctypes.create_unicode_buffer(64); u.GetClassNameW(h, b, 64)
        if b.value == "TMainForm": r.append(h)
        return True
    u.EnumWindows(cb, 0)
    return r[0] if r else None
if __name__ == "__main__":
    h = main()
    u.PostMessageW(h, 0x111, int(sys.argv[1]), 0)
