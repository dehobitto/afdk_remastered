"""Launch dist\AFDK.exe, open Projects\<name>.AFP via the Open dialog, screenshot. usage: open_project.py name out.png [exe]"""
import subprocess, sys, time, os, ctypes
sys.path.insert(0, r"C:\Users\alex\AppData\Local\Temp\claude\C--AFDKdev\dcdcd867-8b6f-4500-926a-c83b2fbbd174\scratchpad\tools")
import ui
name, out = sys.argv[1], sys.argv[2]
exe = sys.argv[3] if len(sys.argv) > 3 else r"C:\AFDKdev\patch\dist\AFDK.exe"
subprocess.run(["taskkill", "/F", "/IM", "AFDK.exe"], capture_output=True)
p = subprocess.Popen([exe], cwd=os.path.dirname(exe))
time.sleep(4)
ui.move(45, 55); ui.mouse(ui.LD); ui.mouse(ui.LU)         # Open toolbar button
time.sleep(1.5)
ui.key(0x41, ctrl=True)                                    # focus filename box text
ui.shot(out.replace(".png", "_dlg.png"))
for ch in name:
    vk = ui.u.VkKeyScanW(ord(ch)); ui.key(vk & 0xff, shift=bool(vk & 0x100))
ui.key(0x0D)
time.sleep(2.5)
ui.shot(out)
print("pid", p.pid)
