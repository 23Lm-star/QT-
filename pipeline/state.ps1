# pipeline/state.ps1 -- xiangmu session-opening readout. The ONLY session-opening action.
$repo = (git rev-parse --show-toplevel)
$branch = git rev-parse --abbrev-ref HEAD
$hash = git rev-parse --short HEAD
$dirty = git status --porcelain
$handoff = Get-ChildItem (Join-Path $repo "project-docs\handoffs") -Filter "*.txt" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$pendingAudit = Get-ChildItem (Join-Path $repo "build\evidence") -Recurse -Filter "audit-report.md" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 3

Write-Output "branch: $branch   head: $hash"
if ($dirty) { Write-Output "SUSPENDED git state (closeout before any node):"; $dirty | Select-Object -First 5 }
else { Write-Output "suspended git state: none" }
if ($handoff) { Write-Output "last handoff: $($handoff.Name)" }
if ($pendingAudit) { Write-Output "pending audit reports:"; $pendingAudit | ForEach-Object { Write-Output "  $($_.FullName.Replace($repo,''))" } }
$wt = git worktree list
Write-Output "worktrees:"; $wt | Select-Object -First 4
