# pipeline/audit.ps1 -- xiangmu boundary + evidence audit. One page PASS/FAIL.
$ErrorActionPreference = "Continue"
$repo = (git rev-parse --show-toplevel)
$hash = git rev-parse --short HEAD
$fail = @()

# 1. Frozen-path boundary check: PDU protocol contract must not change without LARGE approval.
$frozen = @(
    "Client/protocol.h",   # PDU message contract (ENUM_MSG_TYPE_*, struct layout)
    "Server/protocol.h"
)
$changed = git diff --name-only HEAD~1..HEAD 2>$null
if ($changed) {
    foreach ($f in $changed) {
        foreach ($p in $frozen) {
            if ($f -like "$p*") { Write-Output "FROZEN-PATH HIT: $f -> escalate to LARGE + user approval"; $fail += "frozen:$f" }
        }
    }
}

# 2. Repo hygiene: third-party / runtime data must never come back into git
$back = git ls-files | Select-String -Pattern "^Server/qtbase/|^Server/filesys/|\.pro\.user$|^Makefile"
if ($back) { Write-Output "REGRESSION: previously-purged paths tracked again:"; $back | Select-Object -First 5; $fail += "hygiene" }

# 3. Evidence reconciliation: a test log must exist for this hash (bootstrap: 0-test mode counts)
$evDir = Join-Path $repo "build\evidence\current"
$hasLog = Get-ChildItem $evDir -Filter "test-$hash-*.log" -ErrorAction SilentlyContinue
if (-not $hasLog) { Write-Output "NO test evidence for $hash (run pipeline/test.ps1)"; $fail += "evidence" }

if ($fail.Count -gt 0) { Write-Output "audit RED: $($fail -join ', ')"; exit 1 }
Write-Output "audit PASS ($hash)"
exit 0
