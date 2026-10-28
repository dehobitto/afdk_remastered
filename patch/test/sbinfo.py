import ctypes, sys
from ctypes import wintypes as w
u = ctypes.windll.user32
class SI(ctypes.Structure):
    _fields_=[("cbSize",w.UINT),("fMask",w.UINT),("nMin",ctypes.c_int),("nMax",ctypes.c_int),("nPage",w.UINT),("nPos",ctypes.c_int),("nTrackPos",ctypes.c_int)]
res=[]
@ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
def cb(h,l):
    b=ctypes.create_unicode_buffer(64); u.GetClassNameW(h,b,64)
    if b.value in ("TScrollBar","TFrpaint") and u.IsWindowVisible(h):
        r=w.RECT(); u.GetWindowRect(h,ctypes.byref(r))
        si=SI(); si.cbSize=ctypes.sizeof(SI); si.fMask=0x1f
        ok=u.GetScrollInfo(h,2,ctypes.byref(si)) if b.value=="TScrollBar" else 0
        par=u.GetParent(h)
        print(hex(h),b.value,(r.left,r.top,r.right,r.bottom),"parent",hex(par),"range",si.nMin,si.nMax,"page",si.nPage,"pos",si.nPos, "style",hex(u.GetWindowLongW(h,-16)&0xffffffff))
    return True
@ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
def top(h,l):
    p=w.DWORD(); u.GetWindowThreadProcessId(h,ctypes.byref(p))
    if p.value==int(sys.argv[1]): u.EnumChildWindows(h,cb,0)
    return True
u.EnumWindows(top,0)
