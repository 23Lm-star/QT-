# Codex — project entry (Development OS v1.7)

## Development OS v1.7
When the task is governed by Development OS v1.7:
1. The governance layer is C:/Users/王晨扬/.codex/skills/development-os-v1-7/SKILL.md and must be obeyed. If it is already present in the conversation context, do NOT re-read it; read it at most once per session, only when it is missing.
2. Open every session by running: powershell -ExecutionPolicy Bypass -File pipeline/state.ps1
3. Beyond SKILL.md, read only the reference file INDEX.md routes to for the situation at hand; never read skill files wholesale. Canonical session entry = state.ps1 readout + newest handoff (+ project-docs/project-manifest.yaml); do NOT probe for docs/agent.md, _resume.md or other imported entry conventions.
4. On branch takeover, follow references/multi-agent.md (owner_agent protocol).
5. A validation step you cannot execute (GUI, human eyes, credentials, device, incompatible env): at most ONE attempt, then output NEEDS-HUMAN with exact steps and WAIT -- never retry-loop. Two consecutive failures caused by the environment (deps, drivers, local config): close the node's handoff and move the fix to a separate meta session.
Do not apply to projects pinned to another workflow version.
