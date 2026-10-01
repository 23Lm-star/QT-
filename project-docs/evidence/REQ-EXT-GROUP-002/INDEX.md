# REQ-EXT-GROUP-002 证据索引（development 节点）

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
