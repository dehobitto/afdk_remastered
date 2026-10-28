$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$clang = 'C:\tools\llvm-mingw-20261020-msvcrt-x86_64\bin\i686-w64-mingw32-clang.exe'
$dist = Join-Path $here 'dist'
$orig = Join-Path $here 'orig\AFDK.exe'

New-Item -ItemType Directory -Force $dist | Out-Null
& $clang -shared -O2 -s -Wall -o (Join-Path $dist '_afdk_remastered.dll') (Join-Path $here 'src\afdkfix.c') -luser32 -lgdi32 -ladvapi32 -lkernel32
if ($LASTEXITCODE) { throw 'compile failed' }
& $clang -shared -O2 -s -Wall -o (Join-Path $dist '_afdk_remastered_mcp.dll') (Join-Path $here 'src\mcp.c') -luser32 -lgdi32 -ladvapi32 -lkernel32
if ($LASTEXITCODE) { throw 'compile mcp failed' }
# src\wires.c (_afdk_remastered_wires.dll: lines following moved elements) is not built - it did not work well enough
Copy-Item (Join-Path $here 'src\afdk_mcp.py') $dist -Force
python (Join-Path $here 'inject_import.py') $orig (Join-Path $dist 'AFDK.exe') _afdk_remastered.dll Init
if ($LASTEXITCODE) { throw 'inject failed' }
foreach ($f in 'Lib', 'Projects') {
    if (-not (Test-Path (Join-Path $dist $f))) { Copy-Item (Join-Path $root $f) (Join-Path $dist $f) -Recurse }
}
foreach ($f in 'AFDK.hlp', 'AFDK.GID') {
    if (-not (Test-Path (Join-Path $dist $f))) { Copy-Item (Join-Path $root $f) $dist }
}
Write-Host "built -> $dist"
