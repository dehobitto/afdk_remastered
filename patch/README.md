# Patch sources

Build: `powershell -File build.ps1` (needs llvm-mingw; with the original AFDK 2.0 exe in `orig/AFDK.exe` the import is injected again, otherwise the patched `AFDK.exe` from the repo root is used). Version: `VERSION`. Release: push a tag `v<VERSION>`. See [../README.md](../README.md) and [../CLAUDE.md](../CLAUDE.md).
