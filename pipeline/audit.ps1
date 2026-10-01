# pipeline/audit.ps1 -- xiangmu boundary + evidence audit. One page PASS/FAIL.
# v1.7.1: branch-scoped frozen check (merge-base..HEAD), docs-only evidence resolution,
#         advisory codegraph impact face. Repo contract: change only via its own SMALL task.
$ErrorActionPreference = "Continue"
# Repo root from script location, NOT `git rev-parse --show-toplevel` (PS 5.1 codepage + CWD
# fragility: git commands need Set-Location to the repo first -- mirrors pipeline/build.ps1).
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$hash = git rev-parse --short HEAD
$fail = @()

# Trunk base: scope branch-level checks from the fork point to HEAD. On trunk (post-merge)
# merge-base == HEAD, so the range is empty and an already-merged --no-ff merge reports no
# frozen hit (its delta is history, not a new change).
$trunk = "master"
if (-not (git rev-parse --verify --quiet $trunk)) { $trunk = "main" }
$base = (git merge-base $trunk HEAD 2>$null)
if (-not $base) { $base = (git rev-parse HEAD) }   # no trunk -> empty range, frozen check stays silent
$changed = @(git diff --name-only "$base..HEAD" 2>$null)

# 1. Frozen-path boundary check: PDU protocol contract must not change without LARGE approval.
#    Scans the whole branch (base..HEAD), so a frozen change in ANY commit of the branch is a hit,
#    even when the branch tip is a docs commit. Source of truth:
#    project-docs/project-manifest.yaml `frozen_paths:`; hardcoded fallback if unreadable.
$frozen = @()
$manifest = Join-Path $repo "project-docs\project-manifest.yaml"
if (Test-Path $manifest) {
    $inFrozen = $false
    foreach ($line in Get-Content -LiteralPath $manifest) {
        if ($line -match '^frozen_paths:\s*(#.*)?$') { $inFrozen = $true; continue }
        if (-not $inFrozen) { continue }
        if ($line -match '^\s+-\s+([^\s#]+)') { $frozen += $matches[1] }
        elseif ($line -notmatch '^\s*$' -and $line -notmatch '^\s*#') { break }  # dedent ends the list
    }
}
if ($frozen.Count -eq 0) { $frozen = @("Client/protocol.h", "Server/protocol.h") }  # keep in sync with the manifest
foreach ($f in $changed) {
    foreach ($p in $frozen) {
        if ($f -like "$p*") { Write-Output "FROZEN-PATH HIT: $f -> escalate to LARGE + user approval"; $fail += "frozen:$f" }
    }
}

# 2. Repo hygiene: third-party / runtime data must never come back into git
$back = git ls-files | Select-String -Pattern "^Server/qtbase/|^Server/filesys/|\.pro\.user$|^Makefile"
if ($back) { Write-Output "REGRESSION: previously-purged paths tracked again:"; $back | Select-Object -First 5; $fail += "hygiene" }

# 3. Evidence reconciliation: a code commit needs its own test log; a docs-only commit (touches
#    nothing under a source/pipeline root) reuses the last code commit's evidence.
$codeRoots = @("Client", "Server", "pipeline", "src", "include", "tests", "tools")  # Client/Server = this repo's source roots (manifest directory_contract)
$headChanged = @(git diff --name-only HEAD~1..HEAD 2>$null)
$isCode = $false
foreach ($f in $headChanged) { foreach ($r in $codeRoots) { if ($f -like "$r/*") { $isCode = $true } } }
$evHash = $hash
if (-not $isCode) {
    $lastCode = (git log -1 --full-history --format=%h -- $codeRoots 2>$null | Select-Object -First 1)  # --full-history keeps a code-carrying merge as the reference
    if ($lastCode) { $evHash = "$lastCode".Trim(); Write-Output "docs-only commit: evidence from $evHash" }
}
$evDir = Join-Path $repo "build\evidence\current"
$hasLog = Get-ChildItem $evDir -Filter "test-$evHash-*.log" -ErrorAction SilentlyContinue
if (-not $hasLog) { Write-Output "NO test evidence for $evHash (run pipeline/test.ps1)"; $fail += "evidence" }

# 4. Codegraph impact face (advisory -- never changes the verdict; degrades to a skipped line).
#    references/pipeline.md: "codegraph sync + impact face via impact-check.py". The script lives
#    outside the repo (skill checkout), so absence on another machine is expected and non-fatal.
$impact = "skipped: impact-check unavailable"
try {
    $skillScripts = @("$env:USERPROFILE\.claude\skills\development-os-v1-7\scripts", "$env:USERPROFILE\.codex\skills\development-os-v1-7\scripts") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    $ic = if ($skillScripts) { Join-Path $skillScripts "impact-check.py" } else { $null }
    if (-not $ic -or -not (Test-Path $ic)) {
        $impact = "skipped: impact-check unavailable"
    } elseif ($changed.Count -eq 0) {
        $impact = "skipped: no changed paths on branch"
    } else {
        $icExe = $null; $icPre = @()
        foreach ($c in @(@("py", "-3.13"), @("py", "-3"), @("python3", ""), @("python", ""))) {
            if (Get-Command $c[0] -ErrorAction SilentlyContinue) { $icExe = $c[0]; if ($c[1]) { $icPre = @($c[1]) }; break }
        }
        if (-not $icExe) {
            $impact = "skipped: no python interpreter"
        } else {
            $icArgs = $icPre + @($ic, "--path", $repo, "--timeout", "30")
            foreach ($f in $changed) { $icArgs += @("--changed-file", $f) }
            $job = Start-Job -ScriptBlock { param($exe, $a) & $exe @a 2>&1 | Out-String } -ArgumentList $icExe, $icArgs
            if (Wait-Job $job -Timeout 60) {
                $out = Receive-Job $job
                if ($out -match '(?m)^verdict\s*:\s*(\S+)') {
                    $v = $matches[1]
                    if ($v -eq "PASS") { $impact = "PASS" }
                    elseif ($out -match '(?m)^notes\s*:\s*\r?\n\s+-\s+([^\r\n]+)') { $impact = "$v ($($matches[1].Trim()))" }
                    else { $impact = $v }
                } else { $impact = "skipped: unparseable output" }
            } else {
                Stop-Job $job -ErrorAction SilentlyContinue
                $impact = "skipped: timeout"
            }
            Remove-Job $job -Force -ErrorAction SilentlyContinue
        }
    }
} catch { $impact = "skipped: $($_.Exception.Message)" }
Write-Output "impact-check: $impact"

if ($fail.Count -gt 0) { Write-Output "audit RED: $($fail -join ', ')"; exit 1 }
Write-Output "audit PASS ($hash)"
exit 0
