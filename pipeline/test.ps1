# pipeline/test.ps1 -- xiangmu regression (bootstrap stage: no automated suite registered yet)
# No-forged-pass: this script reports honestly that it runs 0 tests until TESTS is filled.
$ErrorActionPreference = "Continue"
$repo = (git rev-parse --show-toplevel)
$hash = git rev-parse --short HEAD
$evDir = Join-Path $repo "build\evidence\current"
New-Item -ItemType Directory -Force -Path $evDir | Out-Null
$log = Join-Path $evDir "test-$hash-$(Get-Date -Format yyyyMMdd-HHmmss).log"

# --- FILL: add automated test executables here as they are created ---
$TESTS = @()

if ($TESTS.Count -eq 0) {
    Write-Output "test: 0 automated tests registered (fill TESTS in pipeline/test.ps1)"
    Write-Output "GUI/manual verification is recorded as human-verified evidence, not BLOCKED."
    "no automated tests registered (hash $hash)" | Out-File $log -Encoding utf8
    exit 0
}

$fail = 0
foreach ($t in $TESTS) {
    if (-not (Test-Path $t)) { Write-Output "MISSING test exe: $t"; $fail++; continue }
    & $t *>> $log
    if ($LASTEXITCODE -ne 0) { Write-Output "FAIL: $t"; $fail++ } else { Write-Output "PASS: $t" }
}
Write-Output "test log -> $log"
if ($fail -gt 0) { Write-Output "test RED ($fail failed)"; exit 1 }
Write-Output "test GREEN"
exit 0
