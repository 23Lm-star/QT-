# REQ-LOG-007 日志分级治理 — 任务记录

## 档位
- **MEDIUM**（新能力，有验收标准 A1–A9；只动生产源码日志语句级别，不触碰 public contract——`protocol.h` 为 frozen_path 且在本需求禁止清单内）。
- 流程版本：Development OS v1.7。分支：`refactor/logging-levels`。

## 依赖核查（2026-09-29 goal 节点）
| 依赖 | 状态 |
|---|---|
| REQ-AUTH-002（必须先合并，改同一批行） | ✅ 已合并 master @`4b3a376`（no-ff），BRANCH-ORDER 标记完成 |
| REQ-FS-003 | ✅ 已合并 @`f937c90`（复审 PASS），其新增校验日志由本需求一并分级 |
| REQ-REPO-006 历史重写 | ✅ 完成；**文档基线 `b5d7bb3` 已不存在**（commit-map 见 REQ-REPO-006 证据），A7 基线改为重测 |
| REQ-DOC-005（并行分支） | ✅ 本会话开场后 master 前进 @`08265d5`（README/LICENSE 合并，与 LOG-007 文件零重叠）——分支从该 HEAD 切出 |

- 未合并且可能冲突的分支：**无**（`chore/history-purge`、`fix/path-traversal-and-ownership`、`docs/repo-chrome`、`add_数据库连接池功能`、`worktree-agent-*` 均 0 ahead 或已合入）。工作区干净。

## A7 重基线（master@08265d5 实测，行计数 `grep -c "qDebug\|qWarning\|qCritical"`）
| 文件 | 文档基线(b5d7bb3) | 当前 | 差异来源 |
|---|---|---|---|
| Server/msghandler.cpp | 77 | **84** | AUTH-002 −3 凭据行；FS-003 新增校验日志 +10 |
| Server/operatedb.cpp | 38 | **36** | AUTH-002 −4 凭据行 |
| 其余 12 文件 | 同文档 | 同文档 | 无变化 |
| **合计** | 213 | **213**（恰好等值） | — |
头文件（*.h）无日志调用。全部 213 处落在允许清单内。

## 决策日志（开发前定型，待主人批准）
- **D1｜分类 C++ 标识符改名**：文档 §3.1 用 `net/db/fs/auth/app` 作标识符，与 `operatedb.cpp`/`databaseconnectionpool.cpp` 中约 20 处局部变量 `QSqlDatabase db` **冲突**（`qCDebug(db,...)` 会被变量遮蔽，编译失败）。改为标识符 `lcNet/lcDb/lcFs/lcAuth/lcApp`（客户端 `lcCli`），**显示名不变**（`xmu.net` 等），`QT_LOGGING_RULES` 规则串完全照文档。
- **D2｜setFilterRules 加环境变量守卫**：文档 §3.2 断言"环境变量覆盖代码设置"——Qt 5.14 中程序化规则与 env 的实际优先级不可靠（版本间行为有出入）。改为：`QT_LOGGING_RULES` 为空时才调用 `setFilterRules("xmu.*.debug=false")`。两种优先级语义下 A3 都成立。开发时按 §7 做 quiet/verbose 双跑实测固化结论。
- **D3｜客户端建 `Client/logging.h`/`Client/logging.cpp`**（单分类 `xmu.cli`，登记入 `Client.pro`），与 Server 对称；弃用文档 §9 的"匿名分类塞 main.cpp"方案。
- **D4｜A6 去路径清单扩大**：文档只点名 file.cpp:411/205/320；实测 `Client/file.cpp` 的 :68、:69、:228、:394 同样打印绝对路径。A6 范围 = 全部路径输出点（去绝对路径、留文件名，级别归 debug）。
- **D5｜Server/mytcpsocket.cpp PDU 转储（~:390-396）含登录包 caData（密码哈希）**：AUTH-002 A6 移交本需求。默认方案：降级 `qCDebug(lcNet)`（默认不输出）+ 保留语句（守 A7）。是否额外涂掉哈希内容属禁止事项 4 的例外 → **需主人裁决**。
- **D6｜过滤规则串**：文档含 `xmu.*.info=true`，info 本就默认开启；建议只保留 `xmu.*.debug=false`（行为等价，字符串更短）。

## allowed paths
文档 §四全部 19 项 + 新增：`Client/logging.h`、`Client/logging.cpp`、`project-docs/tasks/REQ-LOG-007/`、`project-docs/evidence/REQ-LOG-007/`、handoff。禁止事项 1–10 全数继承。

## 验收与证据计划
- A1–A9 照文档 §二；A7 对比基线 = 本文件重基线表（非文档旧表）。
- 人工验证 §7 六项：quiet 启动、`QT_LOGGING_RULES="xmu.*.debug=true"`、单类 `xmu.db.debug=true`、制造配置错误看 qCritical、语句计数、无路径打印。
- 证据目录：`build/evidence/refactor-logging-levels/<hash>/`（pipeline-run / quiet-run / verbose-run / manual-verify / manifest），归档至 `project-docs/evidence/REQ-LOG-007/`。
- 脚本路径修正：skill 脚本实际位于 `C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/`（与文档一致）。

## 流程
goal（本节点）→ 主人批准 → dev（先 Server 后 Client，逐文件改级别；先建分类文件+登记 .pro）→ build/test/audit → §7 人工验证 → 提交 → 等主人批准合并 → no-ff 合 master → handoff。**全程不 push。**

## 状态
**待开发前总结批准，未动任何生产源码。**
