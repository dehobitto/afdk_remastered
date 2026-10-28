"""Launch dist exe with a project, Save As <unicode name> through the dialog, screenshot the result.
usage: savetest.py "name" out.png"""
import subprocess, sys, time, os
sys.path.insert(0, r"C:\Users\alex\AppData\Local\Temp\claude\C--AFDKdev\dcdcd867-8b6f-4500-926a-c83b2fbbd174\scratchpad\tools")
import ui, cmd
name, out = sys.argv[1], sys.argv[2]
subprocess.run(["taskkill", "/F", "/IM", "AFDK.exe"], capture_output=True)
time.sleep(1)
exe = r"C:\AFDKdev\patch\dist\AFDK.exe"
p = subprocess.Popen([exe, r"C:\AFDKdev\patch\dist\Projects\lab2_1.AFP"], cwd=os.path.dirname(exe))
time.sleep(5)
cmd.u.PostMessageW(cmd.main(), 0x111, 5, 0)     # File > Save as
time.sleep(3)
import ctypes
# type unicode text
import ctypes.wintypes as wt
class KI(ctypes.Structure): _fields_=[("wVk",wt.WORD),("wScan",wt.WORD),("dwFlags",wt.DWORD),("time",wt.DWORD),("dwExtraInfo",ctypes.c_void_p)]
class MI(ctypes.Structure): _fields_=[("dx",ctypes.c_long),("dy",ctypes.c_long),("mouseData",wt.DWORD),("dwFlags",wt.DWORD),("time",wt.DWORD),("dwExtraInfo",ctypes.c_void_p)]
class IU(ctypes.Union): _fields_=[("ki",KI),("mi",MI)]
class INP(ctypes.Structure): _fields_=[("type",wt.DWORD),("u",IU)]
for ch in name:
    for fl in (4, 6):
        i = INP(1, IU(ki=KI(0, ord(ch), fl, 0, None))); ui.u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INP))
    time.sleep(0.05)
time.sleep(0.5)
ui.key(0x0D)
time.sleep(3)
ui.shot(out, (0, 0, 1400, 500))
print(os.listdir(r"C:\AFDKdev\patch\dist\Projects"))
