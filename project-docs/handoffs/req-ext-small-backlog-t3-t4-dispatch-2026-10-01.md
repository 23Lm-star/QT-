# 交接词：SMALL 遗留收尾（T3 + T4，同一批次的后两项）

> 用法：新会话首条消息整段粘贴本文。基线 `master@60a5fc9`（以 `state.ps1` 实测为准）。

## 开场（按序）
1. 用 Skill 工具加载 `development-os-v1-7`（治理层）。
2. 跑 `powershell -ExecutionPolicy Bypass -File pipeline/state.ps1`，确认 HEAD 与基线一致。
3. 读 `project-docs/handoffs/req-ext-small-backlog-dispatch-2026-10-01.md` 的「执行台账」（T1、T2 已完成）+ 本文件。

## 本批次已完成（不要重做、不要回退）
- **T1** `small/pipeline-audit-frozen-parse` → 合并 `master@488f964`：F1 **FIXED**（`audit.ps1` 改按 raw UTF-8 读 manifest；主工作树冻结解析 1→2）、F2 **NOT_APPLICABLE**（manifest 声明 `code_intel.impact_status`，未配 test glob 以免造假 PASS）。
- **T2** `small/unify-e2e-runform` → 合并 `master@0d92607`：FINDING-DEV-2 **FIXED**（五套件统一运行形态，重跑 13/43/37/48/46 全绿，规范见 `project-docs/evidence/E2E-RUNFORM.md`）。
- **环境变更（需知悉）**：未跟踪文件 `Server/connect.config` 第 3 行 = `./filesys`，**必须 CRLF**（`server.cpp:25` 按 `\r\n` 切分，LF 会让 `dist/Server.exe` 启动即 segfault）；`dist/Server.exe` 已按此重建。
- **并行开发面**：REQ-EXT-GROUP-003 的 goal 节点已在**另一个独立会话**开启（分支 `codex/req-ext-group-003-group-files`）。遵守 F-6 错峰纪律：同一时刻单一活动开发区。

## 任务（每项一个独立 SMALL 分支：一需求一分支、一提交、pipeline 绿即 no-ff 合并 master、**不 push**、不跨分支合并、不碰冻结面）

### T3 — `small/normalize-logic-keys-predicate`（FINDING-DEV-3）
- **事实**：`OperateDB::normalizeLogicKeys()`（`Server/operatedb.cpp`）的两条 SQL 选择谓词仍把 `/filesys/<owner>/…` 视为 canonical，存量该形态行不会被转换（**数据面未收口**）。当前库实测 `LIKE '/filesys/%'` 计数 = **0**（潜伏未触发）。
- **目标**：谓词与 `isLogicKeyForm` 的判别对齐；补回归夹具覆盖该形态（含幂等重放）。
- **边界**：这是**生产源码**改动，提交前必须 pipeline 绿（`build.ps1` / `test.ps1` / `audit.ps1`）。若实测证明修复超出函数局部（如需改表/改语义）→ **停止并回报**，按 `task.md` 既定预案降级处理，**不要就地扩大范围**。
- **允许的另一结论**：若判定为「当前无触发面、不值得改」，可给 `WONT_FIX` + 理由，并在任务记录记一行；**不强制修**。
- **收口证据**：修复前/后 `LIKE '/filesys/%'` 计数、夹具重放幂等的 DB 输出、分支 `audit.ps1` PASS 日志。

### T4 — 批次收口
全部完成后给出一页汇总：**分支名 / 合并哈希 / 每项结论（FIXED / WONT_FIX / NOT_APPLICABLE + 理由）/ 复跑计数**。

## 环境事实（上一会话实测，直接用，别重新踩坑）
- 构建：`pipeline/build.ps1`（Qt 5.14.2 mingw73_64 + mingw730_64；qmake `DESTDIR=dist`，产物直接落 `dist/`）。`build/build-Server/release/Server.exe` 是陈旧残留，勿用。
- Python 用 **`py -3.13`**；PATH 上的 `python` 是旧 Anaconda，勿用。MySQL CLI `C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe`，库 `mydb`，迁移脚本幂等可重放。
- e2e 统一运行形态：`project-docs/evidence/E2E-RUNFORM.md`（cwd=`<repo>/dist`、`dist/Server.exe`、`filesPath=./filesys`、脚本自管理 Server 生命周期、夹具自建）。T3 是源码改动，通常不需要跑 e2e，但若改了迁移/规范化路径，需跑 `cross_tree_e2e.py`（37/37，覆盖存量键转换与幂等重放）。
- **合并后补证**：merge 提交落 master 后，在该提交上跑一次 `pipeline/test.ps1`（诚实 0，约 1 秒），否则后续 docs-only 提交的 `audit.ps1` 会因找不到 `test-<mergehash>.log` 报 `audit RED: evidence`。
- 遇到无法执行的验证（GUI／人工观察／凭据／设备）：**最多尝试一次**，输出 `NEEDS-HUMAN` + 确切步骤并停下，不要重试循环。

## 收口要求
- 每个分支：≤10 行 handoff（`py -3.13 C:/Users/王晨扬/.claude/skills/development-os-v1-7/scripts/validate-handoff.py <file>` 校验通过）+ 任务记录追加行 → 合并 `master`（no-ff，零冲突）。
- 全程禁止：跨分支合并、改动冻结面（`Client/protocol.h`/`Server/protocol.h`）、`push`。
- 残留观察项：`build/wt-test-group001-repair`（detached HEAD `514d150`）含未提交的 GROUP-001 handoff，待主人裁决；不要自行处理。

## 会话预算
- 软上限 60 请求/会话。本批次只剩 T3 + T4，预计一个会话可完成；若余量不足，先交回 handoff，不要在验证上压缩。
