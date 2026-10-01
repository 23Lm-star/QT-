# pipeline/test.ps1 -- xiangmu regression (bootstrap stage: no automated suite registered yet)
# No-forged-pass: this script reports honestly that it runs 0 tests until TESTS is filled.
$ErrorActionPreference = "Continue"
$repo = (git rev-parse --show-toplevel)
$hash = git rev-parse --short HEAD
$evDir = Join-Path $repo "build\evidence\current"
New-Item -ItemType Directory -Force -Path $evDir | Out-Null
$log = Join-Path $evDir "test-$hash-$(Get-Date -Format yyyyMMdd-HHmmss).log"

# --- Evidence manifest: unconditional, stamped by THIS script at run time (invariant 3)
function Stamp-Evidence([string]$label) {
    $skillScripts = @("$env:USERPROFILE\.claude\skills\development-os-v1-7\scripts", "$env:USERPROFILE\.codex\skills\development-os-v1-7\scripts") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $skillScripts) { Write-Output "WARN: development-os-v1-7 scripts not found - evidence manifest NOT stamped"; return }
    $mStamp = Get-Date -Format "yyyyMMdd-HHmmss"
    python "$skillScripts\record-evidence.py" --evidence-dir "$evDir\manifest-$label-$hash-$mStamp" --repo $repo --label $label $log | Out-Null
}

# --- FILL: add automated test executables here as they are created ---
$TESTS = @()

if ($TESTS.Count -eq 0) {
    Write-Output "test GREEN-WARN: 0 automated tests registered (fill TESTS in pipeline/test.ps1)"
    Write-Output "  any GREEN claim must cite a named probe or human-verified evidence, not this gate"
    "test GREEN-WARN (0 automated tests, hash $hash)" | Out-File $log -Encoding utf8
    Stamp-Evidence "test"
    exit 0
}

$fail = 0
foreach ($t in $TESTS) {
    if (-not (Test-Path $t)) { Write-Output "MISSING test exe: $t"; $fail++; continue }
    & $t *>> $log
    if ($LASTEXITCODE -ne 0) { Write-Output "FAIL: $t"; $fail++ } else { Write-Output "PASS: $t" }
}
Write-Output "test log -> $log"
if ($fail -gt 0) { "test RED ($fail failed)" | Add-Content $log; Stamp-Evidence "test"; Write-Output "test RED ($fail failed)"; exit 1 }
"test GREEN: hash=$hash" | Add-Content $log
Stamp-Evidence "test"
Write-Output "test GREEN"
exit 0
