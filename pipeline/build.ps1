# pipeline/build.ps1 -- xiangmu build (Qt 5.14.2 MinGW, shadow build into build/)
# NOTE: never use the Anaconda qmake on PATH -- explicit toolchain paths below.
param([string]$Target = "all")
$ErrorActionPreference = "Continue"   # PS5.1: native stderr must not become terminating errors
$repo = (git rev-parse --show-toplevel)
$hash = git rev-parse --short HEAD
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$evDir = Join-Path $repo "build\evidence\current"
New-Item -ItemType Directory -Force -Path $evDir | Out-Null
$log = Join-Path $evDir "build-$hash-$stamp.log"

# --- Evidence manifest: unconditional, stamped by THIS script at run time
# (invariant 3: evidence born trustworthy). RED runs are stamped too -- a failure
# is evidence. Missing skill checkout degrades to a console warning, never a skip.
function Stamp-Evidence([string]$label) {
    $skillScripts = @("$env:USERPROFILE\.claude\skills\development-os-v1-7\scripts", "$env:USERPROFILE\.codex\skills\development-os-v1-7\scripts") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $skillScripts) { Write-Output "WARN: development-os-v1-7 scripts not found - evidence manifest NOT stamped"; return }
    $mStamp = Get-Date -Format "yyyyMMdd-HHmmss"
    python "$skillScripts\record-evidence.py" --evidence-dir "$evDir\manifest-$label-$hash-$mStamp" --repo $repo --label $label $log | Out-Null
}

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
    if ($LASTEXITCODE -ne 0) { Pop-Location; "build RED: qmake $name" | Add-Content $log; Stamp-Evidence "build"; Write-Output "build RED (qmake $name)"; exit 1 }
    cmd /c "`"$MINGWBIN\mingw32-make.exe`" -j4 2>&1"
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) { "build RED: make $name rc=$rc" | Add-Content $log; Stamp-Evidence "build"; Write-Output "build RED (make $name, rc=$rc)"; exit 1 }
    "build OK: $name -> $bld" | Add-Content $log
    Write-Output "build OK: $name -> $bld"
}
"build GREEN: hash=$hash config=$stamp" | Add-Content $log
Stamp-Evidence "build"
Write-Output "build GREEN ($hash, $stamp)"
exit 0
