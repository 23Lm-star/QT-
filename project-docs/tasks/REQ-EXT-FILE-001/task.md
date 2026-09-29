# REQ-EXT-FILE-001 文件资产与内容寻址 — 任务记录

## 档位
- **LARGE**（goal 定档 2026-09-29，owner 同会话批准）：文件存储架构分离（物理对象 vs 逻辑条目）；DB 新增 4 表 + 存量迁移；权限边界收口（对象路径/状态只由服务端生成）；是 FILE-002/GROUP-003 的地基。需求文档自声明 LARGE。
- 硬约束：两 `protocol.h` 零命中（不新增秒传 PDU、不改分块协议——§6）；客户端零改动（D4）。
- 流程版本：Development OS v1.7。分支：`codex/req-ext-file-001-file-assets`，基线 master@`1570744`（已含 SEC-001@`7fc47c1`）。dev worktree：`build/wt-claude`（主工作树由 group-002 占用，不抢占）。
- 节点计划：goal（完成，批准即签核）→ design（`design.md` 随本记录落分支）→ development → independent test（新会话）→ repair → independent audit（新会话、只读）→ integration（三事实行）→ merge no-ff。

## 依赖核查（2026-09-29 实测）
| 依赖 | 状态 |
|---|---|
| REQ-EXT-SEC-001 | ✅ merged master@`7fc47c1`（审计 ACCEPT，48/48 e2e）。`MsgHandler::m_actorName`（连接级）、`actorUserRoot()`=`filesPath/<actor>`、`pathInActorRoot()`、`isPathInSandbox()` 可直接复用；T5 权限挂接以其为唯一身份来源 |
| 并行冲突面 | group-002 仅 goal 记录未开发（依赖 GROUP-001 未启动），源码零重叠；本分支为唯一活动开发区 |

## 决策（owner 批准，2026-09-29，D1–D4 按推荐）
- **D1** 迁移=独立 SQL 脚本（`Server/migrate_ext_file_001.sql`）+ 服务端启动 `schema_version` 版本检查（缺表/低版本 → 拒绝启动并明确日志，不静默建表改数据）。
- **D2** 物理布局=`Server/filesys/objects/{sha256前2位}/{object_id}` 内容寻址；旧路径经 `file_migration_map` 映射保留。
- **D3** 引用归零仅标 `pending_reclaim`（可恢复），本分支不做物理删除。
- **D4** 客户端零改动：所有兼容在旧路径字符串 ↔ `user_file` 逻辑条目之间的服务端双向转换层完成。

## 现状盘点（代码事实，master@1570744）
- 文件入口（`Server/msghandler.cpp`）直接操作**物理目录树**：`MKDIR`/`deldir`/`flushfile`/`renamefile`/`movefile` 全走 `QDir/QFile`，`flushfile` 以物理 `entryInfoList` 渲染 `FileInfo{caName,uiType}` 回显——同内容多副本、无引用概念。
- 上传：`uploadfileinit`→`ResumableUploadSession`（连接内存态，`stagingFilePath` 临时文件）→`UPLOADFILE_DATA` 分块 + `sendUploadAck(offset)`；会话不落库，断连即失（仅靠磁盘 staging 续传）。
- 分享（`sharefile`）：源以 canonical 物理路径登记握手（SEC-001 M-1/B-1 已收口"只能分享本人用户根内文件"）。
- `isPathInSandbox` 要求目标 canonical 存在——迁到逻辑条目后"待建目录不存在"会误拒，T3 需拆出逻辑路径校验（语法归一+actor 根约束），物理校验保留给仍走磁盘的操作（纵深防御）。
- DB：`OperateDB` 单例 + `DatabaseConnectionPool`（RAII）；建表均为手工 `migrate_*.sql`，无版本检查机制——T1 引入 `schema_version`。
- 启动链：`Server::initialize()`→loadconfig→listen；`OperateDB::connect()` 在连接池初始化处调用——版本检查挂在此后。

## 开发任务分解（T1–T8，已批准）
- **T1** 数据模型与迁移：4 表 DDL（`file_object`/`user_file`/`upload_session`/`file_migration_map`）+ `schema_version` 单行表；`migrate_ext_file_001.sql` 幂等（`IF NOT EXISTS`/唯一键）；启动版本检查 fail-fast；回滚=只建新表不动旧数据，DROP 脚本随行。
- **T2** 对象存储层：新模块 `Server/fileobjectstore.{h,cpp}`——服务端生成 `objects/{h2}/{object_id}` 路径；staging 写满→服务端 SHA-256 复算→同卷原子 rename 转正；`(owner,parent,name)` 命中同摘要对象=秒传地基（本分支不开 PDU）；引用计数±与 `pending_reclaim` 状态机。**对象命中+条目创建+计数变更同事务**（连接池取同一连接执行）。
- **T3** 逻辑条目收编：`MKDIR/deldir/flushfile/renamefile/movefile` 改为操作 `user_file`（目录=is_dir 行，object_id NULL）；`flushfile` 从表渲染（`FileInfo` 布局不变，展示名截 32 与现状一致）；同用户同目录同名同对象幂等；同名不同内容按 `(1)(2)` 规则生成；客户端伪造物理路径失效（路径只是逻辑键）。
- **T4** 上传会话落库：`uploadfileinit/UPLOADFILE_DATA/完成` 三阶段把 offset/过期/临时路径持久化 `upload_session`；staging 归 `filesys/tmp/`；重启/断连后可按表恢复续传；分块 PDU 不动。
- **T5** 权限挂接：owner 一律 `m_actorName`；客户端摘要/大小仅预检元数据，服务端复算不一致→不转正、条目不建、拒绝日志（`lcFs/lcAuth`，不落内容）；非本人对象/条目查询全以 `(owner,...)` 谓词收口；`sharefile` 源改查 `user_file` 归属（替代 canonical 物理比对），握手授权语义不回退。
- **T6** 存量迁移与兼容：`migrate_ext_file_001_data`（按 `file_migration_map` 幂等：哈希→建对象→建条目→记映射；重跑跳过；同名不同内容不覆盖，冲突时保留旧文件原样并记录）；旧目录不删；旧路径字符串↔逻辑条目转换覆盖上传/下载/列表/删除/重命名/移动/分享全回归面。
- **T7** 验收测试与证据：验收 1–5 端到端脚本（仿 SEC-001 48/48 模式：Python 构造 PDU + 服务端拒绝日志断言 + DB 状态断言）；pipeline build/test/audit；证据绑定候选哈希。**停止条件**：旧路径与新资产建不出可审计双向映射（映射表有缺/歧义）→ 停，不进 FILE-002/GROUP-003。
- **T8** 收尾：任务记录/证据归档 `project-docs/`；BRANCH-ORDER E 阶段登记；handoff。

## 允许路径
`Server/fileobjectstore.{h,cpp}`、`Server/operatedb.{h,cpp}`、`Server/resumableuploadsession.{h,cpp}`、`Server/uploadwritetask.{h,cpp}`、`Server/msghandler.{h,cpp}`、`Server/mytcpsocket.{h,cpp}`（会话持久化配合）、`Server/server.{h,cpp}`（启动版本检查）、`Server/Server.pro`、`Server/migrate_ext_file_001.sql`、`Client/*`（仅回归必需的最小配合，预期零改动）、`project-docs/tasks|evidence/REQ-EXT-FILE-001/`。
**禁止**：两 `protocol.h`（frozen）、`Server/filesys/` 入库、新外部依赖、顺手实现秒传 PDU/群文件/乱序并发上传、物理删除用户旧文件。

## 关键风险（design 对照）
1. `flushfile` 从 DB 渲染后，未迁移目录显示为空 → T6 先迁移后切换渲染，或渲染层 DB 优先 + 物理回退（双读过渡，本分支取后者更稳）。
2. 32 字节展示名 + `(1)` 后缀溢出 → 后缀插入时截基名保留扩展名，规则写进 T3 测试。
3. MySQL 事务与连接池：引用计数并发必须同连接 `SELECT...FOR UPDATE` + `COMMIT`；池按线程隔离，单连接对象操作串行化在 socket 线程内（现状 handlMsg 即同线程）。
4. 下载走物理路径处（`downloadfile`/分享回退）需经 `object storage_path` 解析，保持 `pathInActorRoot` 式校验不放松。
