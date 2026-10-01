# REQ-EXT-GROUP-002 证据索引

## independent audit 节点（2026-10-01，候选 03dc01d）

目录 `audit/`：`report.md`（**结论 PASS**：0 条阻塞缺陷；冻结路径仅追加 88–96 复证、三门与证据对账、
`FINDING-PRE1` 属基线既有之独立验证、CodeGraph 影响闭包）、`independent-verification.log`（复证命令与原始输出）、
`branch.diff`（审计基线差异）、`audit-run-fresh.txt` / `audit-run-with-evidence.txt`（`pipeline/audit.ps1`
两次运行：干净工作树 RED=证据未运输 → 运输 test 日志后 `PASS (03dc01d)`）、`impact-check{.json,-denied.txt}`、
`manifest/manifest.txt`（record-evidence 盖章；其 `commit:` 为**归档时主工作树 HEAD**，运行绑定哈希见
test 工作树内 manifest 与文件名）。
运输副本（供 owner 侧 state.ps1 发现）：`build/evidence/codex-req-ext-group-002-group-messaging/03dc01d86a3…/audit-report.md`。

## independent test 节点（2026-10-01，候选 03dc01d）

目录 `independent-test/`：`report.md`（结论/矩阵/FINDING-PRE1）、自研 harness
（`itest_group002.py` part1、`itest_part2.py` part2、`itest_part3.py` part3、`wire.py` 共享线协议、
`probe_chatmsg.py` 既有缺陷最小复现）、运行日志 `part{1,2,3}-run.log`、`group001-regress.log`、
机器可读 `itest_results_part{1,2,3}.json`、服务端控制台 `server-console.log`、pipeline 构建/测试日志。
结果：part1 **58/58**、part2 **36/37**、part3 **13/13**、GROUP-001 回归 **15/15**；
唯一 FAIL = 基线既有缺陷 FINDING-PRE1（`getchatMsg` 与 master 字节一致，非本候选引入）。

## development 节点（2026-10-01，候选 03dc01d）

| 文件 | 节点 | 结论/内容 | 锚定哈希 |
|---|---|---|---|
| verify.py | development | T7 端到端 harness（38 用例：幂等/序号/权限三断/分页边界/有界广播/回归） | — |
| verify-output.log | development | 终轮 **38/38 PASS** | 03dc01d |
| evidence.json | development | harness 机器可读结果（含 commit 字段） | 03dc01d |
| server-console.log | development | 终轮服务端控制台（含 P7 丢弃告警 [GROUP] drops） | 03dc01d |
| pipeline-build-03dc01d.log | development | 全量构建 GREEN | 03dc01d |
| manifest.txt | — | record-evidence.py 原生盖章（sha256 清单） | 03dc01d |

复现步骤（独立测试会话）：
1. 先迁移数据库：`mysql -uroot -p123456 mydb < Server/migrate_ext_group_002.sql`（幂等附加式；本地库已执行）。
2. Release 构建产物在 `dist/`；以 `QT_ASSUME_STDERR_HAS_CONSOLE=1 QT_FORCE_STDERR_LOGGING=1 ./Server.exe > server-console.log 2>&1` 启动（工作目录 = dist）。
3. `python verify.py` 需 pymysql（用 Python313，非 Anaconda），依赖服务端已启动；harness 自带用户/群初始化，可重复执行。

P6 分页探针在第一轮抓住真实缺陷（修正前 793/800：前向翻页跳过边界消息、后向翻页原地踏步），修复提交 `03dc01d`，本目录证据为修复后终轮。
