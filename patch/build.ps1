$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
# llvm-mingw clang (i686); CI passes its own path in AFDK_CLANG
$clang = if ($env:AFDK_CLANG) { $env:AFDK_CLANG } else { 'C:\tools\llvm-mingw-20261020-msvcrt-x86_64\bin\i686-w64-mingw32-clang.exe' }
$dist = Join-Path $here 'dist'
$orig = Join-Path $here 'orig\AFDK.exe'
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()

New-Item -ItemType Directory -Force $dist | Out-Null
Set-Content -Encoding ascii (Join-Path $dist 'version.h') "#define AFDK_VERSION `"$version`""
& $clang -shared -O2 -s -Wall -I $dist -o (Join-Path $dist '_afdk_remastered.dll') (Join-Path $here 'src\afdkfix.c') -luser32 -lgdi32 -ladvapi32 -lkernel32
if ($LASTEXITCODE) { throw 'compile failed' }
& $clang -shared -O2 -s -Wall -I $dist -o (Join-Path $dist '_afdk_remastered_mcp.dll') (Join-Path $here 'src\mcp.c') -luser32 -lgdi32 -ladvapi32 -lkernel32
if ($LASTEXITCODE) { throw 'compile mcp failed' }
# src\wires.c (_afdk_remastered_wires.dll: lines following moved elements) is not built - it did not work well enough
$py = [IO.File]::ReadAllText((Join-Path $here 'src\afdk_mcp.py')).Replace('__version__ = "dev"', "__version__ = `"$version`"")
[IO.File]::WriteAllText((Join-Path $dist 'afdk_mcp.py'), $py)

# AFDK.exe: the original with the DLL import added
python (Join-Path $here 'inject_import.py') $orig (Join-Path $dist 'AFDK.exe') _afdk_remastered.dll Init
if ($LASTEXITCODE) { throw 'inject failed' }
foreach ($f in 'Lib', 'Projects', 'AFDK.hlp', 'AFDK.GID') {
    if ((Test-Path (Join-Path $root $f)) -and -not (Test-Path (Join-Path $dist $f))) { Copy-Item (Join-Path $root $f) (Join-Path $dist $f) -Recurse }
}
Write-Host "built $version -> $dist"
