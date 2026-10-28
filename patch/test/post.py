import sys, ctypes
sys.path.insert(0, r"C:\AFDKdev\patch\test")
import cmd
cmd.u.PostMessageW(cmd.main(), 0x8000 + int(sys.argv[1], 0), 0, 0)
