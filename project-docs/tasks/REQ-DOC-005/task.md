# REQ-DOC-005 任务记录 — 仓库门面：README + LICENSE

| 项目 | 值 |
|---|---|
| 档位 | SMALL（纯新增文档，单提交可回退，不触生产契约、不推远端） |
| 依据 | 需求文档 `project-docs/requirements/REQ-DOC-005-repo-chrome.md` §一（门面缺失非代码缺陷） |
| 分支 | `docs/repo-chrome`（基线 master @082ebd9，较文档基线 b5d7bb3 前移） |
| owner_agent | claude-code |

## 验收标准（对照需求 §二）

- A1/A2 README 存在、含依赖/构建/运行三步，按步构建成功 → 见 evidence/pipeline-run.log
- A3 LICENSE 存在且为标准全文（MIT）
- A4 学习项目免责声明置顶
- A5 connect.config 复制说明（与 REQ-REPO-004 模板呼应）
- A6 遗留分支处置有明确建议（本记录决策日志，不删任何分支）
- A7 pipeline 三门 → build GREEN / test 如实报告 / audit 见 O-4 备注

## Allowed paths

README.md、LICENSE、project-docs/tasks/REQ-DOC-005/task.md、project-docs/evidence/REQ-DOC-005/
（`.gitattributes` 经批准本次不加）

## 决策日志

1. **LICENSE = MIT**：主人 2026-09-29 批准卡选定；版权署名 `wcy`（git user，可后改）。
2. **`.gitattributes` 不加**：主人批准；本机 `core.autocrlf=true`，模板中 `eol=lf` 必然触发大面积伪 modified（§3.4 回退条款大概率命中）。行尾归一另开独立 SMALL 需求（需 `git add --renormalize` 单独评估）。
3. **遗留分支处置建议（只建议、零删除，删除属不可逆须主人逐个批准）**：
   | 分支 | 现状（2026-09-29 实测） | 建议 | 理由 |
   |---|---|---|---|
   | `origin/main` | 已废弃，远端 HEAD→master | 保留不动 | 删远端不可逆；不影响默认分支观感 |
   | `backup-runnable-2026-07-30`（本地+远端） | **实测已完全并入 master**（`git branch --merged` 确认） | 主人批准后可删 | "可运行备份"价值已被 master 覆盖，"删了没退路"理由不再成立 |
   | `origin/add_数据库连接池功能` | 远端存在 | 保留待议 | 功能（连接池）已在主干实现，疑似历史分支，主人确认后删 |
   | `fix/path-traversal-and-ownership`（本地） | 已并入 master（REQ-FS-003 @f937c90） | 主人批准后可删 | 死分支，需求文档 §3.3 未覆盖（编写后新增） |
   | `worktree-agent-ac6d9e68`（本地） | 已并入 master，worktree 已不存在 | 主人批准后可删 | 会话残留死分支，同上为文档未覆盖项 |
4. **audit.ps1 在 docs 提交上的已知误报（O-4）**：仓库版未实现 v1.7 docs-only 证据回溯，直接跑会报缺证据；处置=如实记录，工具修复另开需求，不在本分支动 `pipeline/`。
5. README 技术栈修正：需求模板"自研内存池"实测代码中无内存池（仅对象池/memalloc），按禁令 #7 以代码为准。
6. `active-locks.yaml` 未登记：SMALL 单人单分支，写所有权由分支+handoff 记录，且不在 allowed paths 内。

## 完成判据

README 步骤实测可构建；全文无真实凭据/IP/密钥；`git branch -a` 与执行前一致；未 push。
