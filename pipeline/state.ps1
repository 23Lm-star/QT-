# pipeline/state.ps1 -- xiangmu session-opening readout (rev v1.7.2). The ONLY
# session-opening action.
# v1.7.2: multi-worktree aware -- follows the newest handoff's branch into the worktree
#         that owns it; pending audit reports filtered by disposition resolved|accepted,
#         capped at newest 3 with total count.
# Repo root from script location, NOT `git rev-parse --show-toplevel` (PS 5.1 codepage issue).
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo  # git commands must run against this repo, not the caller's CWD
$branch = git rev-parse --abbrev-ref HEAD
$hash = git rev-parse --short HEAD
$dirty = git status --porcelain
$handoff = Get-ChildItem (Join-Path $repo "project-docs\handoffs") -Filter "*.txt" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$auditDisp = "none"; $hBranch = ""; $hTier = ""; $hNode = ""; $hOwner = ""
if ($handoff) {
    $hl = Get-Content -LiteralPath $handoff.FullName
    $hTier  = ($hl | Where-Object { $_ -match '^tier:' }   | Select-Object -First 1) -replace '^tier:\s*', ''
    $hNode  = ($hl | Where-Object { $_ -match '^node:' }   | Select-Object -First 1) -replace '^node:\s*', ''
    $hOwner = ($hl | Where-Object { $_ -match '^owner:' }  | Select-Object -First 1) -replace '^owner:\s*', ''
    $hBranch= ($hl | Where-Object { $_ -match '^branch:' } | Select-Object -First 1) -replace '^branch:\s*', ''
    $auditDisp = ($hl | Where-Object { $_ -match '^audit:' } | Select-Object -First 1) -replace '^audit:\s*', ''
    if (-not $auditDisp) { $auditDisp = "none" }
}
Write-Output "branch: $branch   head: $hash"
if ($dirty) { Write-Output "SUSPENDED git state (closeout before any node):"; $dirty | Select-Object -First 5 }
else { Write-Output "suspended git state: none" }
if ($handoff) { Write-Output "last handoff: $($handoff.Name)" }
# Task-worktree section: the active task's branch often lives in build/wt-*, not here.
if ($hBranch -and $hBranch -ne $branch -and $hBranch -ne 'master' -and $hBranch -ne 'main') {
    $wtLine = git worktree list | Where-Object { $_ -match [regex]::Escape($hBranch) } | Select-Object -First 1
    if ($wtLine) {
        $wtPath = $wtLine.Trim() -split '\s+' | Select-Object -First 1
        $wtHash = git -C $wtPath rev-parse --short HEAD 2>$null
        Write-Output "task branch: $hBranch   head: $wtHash   worktree: $wtPath"
    } else {
        Write-Output "task branch: $hBranch NOT checked out in any worktree"
    }
    Write-Output "tier: $hTier   node: $hNode   owner: $hOwner   audit: $auditDisp"
} elseif ($handoff) {
    Write-Output "tier: $hTier   node: $hNode   owner: $hOwner   audit: $auditDisp"
} else {
    Write-Output "tier: SMALL   node: bootstrap   owner: codex"
}
Write-Output "pipeline: code commits rerun the gate; docs-only commits reuse last code evidence"
if ($auditDisp -notmatch '^(resolved|accepted)') {
    $allReports = Get-ChildItem (Join-Path $repo "build\evidence") -Recurse -Filter "audit-report*.md" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    if ($allReports) {
        Write-Output "pending audit reports ($($allReports.Count) total, newest 3):"
        $allReports | Select-Object -First 3 | ForEach-Object { Write-Output "  $($_.FullName.Replace($repo,''))" }
    }
}
$wt = git worktree list
Write-Output "worktrees:"; $wt | Select-Object -First 4
