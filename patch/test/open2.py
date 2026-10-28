r"""Launch dist\AFDK.exe with Projects\<name> on the command line. usage: open2.py name [exe]"""
import subprocess, sys, time, os
sys.path.insert(0, r"C:\Users\alex\AppData\Local\Temp\claude\C--AFDKdev\dcdcd867-8b6f-4500-926a-c83b2fbbd174\scratchpad\tools")
import ui
name = sys.argv[1]
exe = sys.argv[2] if len(sys.argv) > 2 else r"C:\AFDKdev\patch\dist\AFDK.exe"
subprocess.run(["taskkill", "/F", "/IM", "AFDK.exe"], capture_output=True)
time.sleep(0.5)
p = subprocess.Popen([exe, os.path.join(os.path.dirname(exe), "Projects", name)], cwd=os.path.dirname(exe))
time.sleep(5)
ui.focus_afdk()
print("pid", p.pid)
