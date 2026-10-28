import ctypes, sys
from ctypes import wintypes as w
u = ctypes.windll.user32
class SI(ctypes.Structure):
    _fields_=[("cbSize",w.UINT),("fMask",w.UINT),("nMin",ctypes.c_int),("nMax",ctypes.c_int),("nPage",w.UINT),("nPos",ctypes.c_int),("nTrackPos",ctypes.c_int)]
pid=int(sys.argv[1]); bars=[]; cv=[]
@ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
def cb(h,l):
    b=ctypes.create_unicode_buffer(64); u.GetClassNameW(h,b,64)
    if b.value=="TScrollBar" and u.IsWindowVisible(h): bars.append(h)
    if b.value=="TFrpaint" and u.IsWindowVisible(h): cv.append(h)
    return True
@ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
def top(h,l):
    p=w.DWORD(); u.GetWindowThreadProcessId(h,ctypes.byref(p))
    if p.value==pid: u.EnumChildWindows(h,cb,0)
    return True
u.EnumWindows(top,0)
def pos(h):
    si=SI(); si.cbSize=ctypes.sizeof(SI); si.fMask=0x1f; u.GetScrollInfo(h,2,ctypes.byref(si)); return si.nPos,si.nMax,si.nPage
for b in sorted(set(bars)):
    vert = u.GetWindowLongW(b,-16)&1
    par=u.GetParent(b)
    msg=0x115 if vert else 0x114
    for code,name in ((1,'LINEDOWN'),(3,'PAGEDOWN'),(0,'LINEUP'),(2,'PAGEUP')):
        before=pos(b)[0]
        u.SendMessageW(par,msg,code,b)
        print(hex(b),'V' if vert else 'H',name,before,'->',pos(b)[0],pos(b))
