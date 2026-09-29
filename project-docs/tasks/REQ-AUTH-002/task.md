# 任务记录：REQ-AUTH-002 password-salt

- **tier**: MEDIUM
- **tier 依据**: 触碰生产源码（`Server/operatedb.cpp`、`Server/msghandler.cpp`）但不触碰 `frozen_paths`（两个 `protocol.h`）、不改对外协议、不引入新外部依赖、不新建功能；单分支单主题可回滚；需求含 A1–A7 验收标准
- **owner_agent**: claude-code
- **date**: 2026-09-29
- **状态**: **GOAL 完成（需求分析 + 分支创建 + 开发前归纳），等待主人批准后进入 dev**

## 需求（业务语言）

两个互相关联的安全缺陷合并为一个需求（同改 `handleLogin` 一段代码，拆开做必然冲突）：

1. **密码哈希无盐**：数据库存的是客户端裸 MD5，等于待破解的明文等价物，彩虹表第一条即可命中（`init_test_data.sql` 自身就用 `123456` 的 MD5 当测试数据证明了这点）。
2. **凭据写进日志**：7 处排查期遗留 DIAG `qDebug` 把用户名/密码哈希/整个 64 字节凭据区写进日志文件，日志会被采集上报，等于凭据二次泄露。

## 验收标准

见 `project-docs/requirements/REQ-AUTH-002-password-salt.md` §二（A1–A7）。执行中如修订以需求文档为准。

## allowed paths

- `Server/operatedb.cpp`（加盐函数 + 注册/登录/注销三入口）
- `Server/operatedb.h`（仅当需要新增函数声明；若用匿名 namespace 静态函数则不动）
- `Server/msghandler.cpp`（仅删除 222–224 行 DIAG 日志）
- `Server/migrate_add_password_salt.sql`（新建迁移文件）
- `project-docs/tasks/REQ-AUTH-002/task.md`（本文件）
- `project-docs/evidence/REQ-AUTH-002/`
- **待批准追加**：`project-docs/handoffs/fix-auth-security-baseline-*.txt`（§8.2 要求产出，§4 清单遗漏）
- **待批准追加**：`project-docs/init_test_data.sql`（建表与种子数据补 salt 列，见发现②）
- **待批准追加**：`project-docs/code-map.md` 数据模型表中 `user_info` 一行补 `salt`（文档同步，最小改）
- **待批准追加**：`project-docs/requirements/BRANCH-ORDER.md` 更正 ① 的完成标记（见发现①）

## 不做的事（划界）

- 不改协议（`protocol.h` frozen_paths）、不改客户端（`Client/client.cpp` 哈希逻辑保持裸 MD5）
- 不新建改密功能（已核实全局无改密入口）
- 不做日志分级（REQ-LOG-007 独立分支）、不顺手改其他缺陷
- 不 `git push`（推送由主人批准）

## 决策日志（append-only）

| 日期 | 决策 | 说明 |
|---|---|---|
| 2026-09-29 | 需求分析：逐条核实文档断言 vs 实际代码，**全部属实** | 7 处 DIAG 行号全命中（`msghandler.cpp` 222/223/224、`operatedb.cpp` 229/230/237/239）；`client.cpp:23` 裸 MD5、351/377/458 三个调用点命中；`operatedb.cpp` 注册 42 / 登录 214 / 注销 545，注销两处 `pwd=?` 比对在 561、591 行命中；协议 `caData[64]`=用户名 32 + 哈希 32 命中；全局搜索确认无改密功能；`salt` 在源码中零出现（本需求确实未做过） |
| 2026-09-29 | 基线核对：需求文档写 `b5d7bb3`，实际从当前 master `24967df` 切出 | `b5d7bb3` 是祖先；FS-003 已合并但目标文件行号未漂移（DIAG 块仍在 222–224/229–239，注销比对仍在 561/591） |
| 2026-09-29 | **发现①：BRANCH-ORDER.md 误记 ① AUTH-002 已完成，实为未做** | 第 13/87 行引用 `a621309 + a9d06a7`，实为 `fix-db-credentials-out-of-index`（取消跟踪 `Server/connect.config`）的提交。master 上无 salt、7 处 DIAG 仍在，故 ① 实为待执行。建议更正标记（待批准） |
| 2026-09-29 | **发现②：`init_test_data.sql` 重建 user_info 时无 salt 列，存在"新环境装不上"隐患** | 迁移 ALTER 之后若再跑该脚本（DROP+CREATE+INSERT），代码报 `Unknown column 'salt'`，注册/登录/注销全断。建议同步补 `salt VARCHAR(32) NOT NULL DEFAULT ''`（待批准） |
| 2026-09-29 | **发现③：数据模型文档需同步** | `code-map.md:127` user_info 行需补 salt；`interfaces.md:167` 的表结构已陈旧（VARCHAR(255)、friend 列名与现状不符）属既有偏差，不在本需求修 |
| 2026-09-29 | **优化建议（安全，待主人取舍）**：盐熵源用 `QRandomGenerator::system()`（OS CSPRNG）替代文档示例的 `global()` | 同文件同函数、零额外成本；不改变文档定型方案的结构。PBKDF2 迭代（`QPasswordDigestor`，输出截 32 hex）可进一步抗离线爆破，但改动更大，默认不做 |
| 2026-09-29 | **实现细节预警（dev 时注意）**：登录升级盐后，`UPDATE online=1` 的 `pwd=?` 必须绑定"升级后的存储值" | 否则老用户升级后 online 更新匹配 0 行（登录成功但显示离线）；注销同理，两处比对都要用"取 salt → 算加盐哈希"的值 |
| 2026-09-29 | 档位定为 MEDIUM | 判据见上；不触 frozen_paths、不改协议、无外部依赖 |
| 2026-09-29 | 分支 `fix/auth-security-baseline` 从 master `24967df` 切出 | 需求文档本身已在 master 跟踪（73d85d8 入库），分支首个提交为本任务记录 |
| 2026-09-29 | 环境核实：本机 MySQL 5.7 运行中（`mydb`@localhost:3306），`mysql.exe` 可用 | A1–A5 人工验证可行；`pipeline/test.ps1` 为 0 注册测试，运行时行为必须靠人工/端到端验证（沿用 FS-003 的 Python TCP harness 先例） |
