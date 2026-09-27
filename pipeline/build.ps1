# pipeline/build.ps1 -- xiangmu build (Qt 5.14.2 MinGW, shadow build into build/)
# NOTE: never use the Anaconda qmake on PATH -- explicit toolchain paths below.
param([string]$Target = "all")
$ErrorActionPreference = "Continue"   # PS5.1: native stderr must not become terminating errors
$repo = (git rev-parse --show-toplevel)
$hash = git rev-parse --short HEAD
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"

$QTBIN  = "C:\Qt\5.14.2\mingw73_64\bin"
$MINGWBIN = "C:\Qt\Tools\mingw730_64\bin"
$env:Path = "$QTBIN;$MINGWBIN;$env:Path"

$targets = @("Client\Client.pro", "Server\Server.pro")
if ($Target -ne "all") { $targets = @($Target) }

foreach ($pro in $targets) {
    $name = [IO.Path]::GetFileNameWithoutExtension($pro)
    $src  = Join-Path $repo $pro
    $bld  = Join-Path $repo "build\build-$name"
    New-Item -ItemType Directory -Force -Path $bld | Out-Null
    Push-Location $bld
    cmd /c "`"$QTBIN\qmake.exe`" `"$src`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Output "build RED (qmake $name)"; exit 1 }
    cmd /c "`"$MINGWBIN\mingw32-make.exe`" -j4 2>&1"
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) { Write-Output "build RED (make $name, rc=$rc)"; exit 1 }
    Write-Output "build OK: $name -> $bld"
}
Write-Output "build GREEN ($hash, $stamp)"
exit 0
