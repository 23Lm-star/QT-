# REQ-EXT-FILE-002 顺序上传秒传与去重 — 任务记录

## 档位
- **LARGE**（goal 定档 2026-10-01）。理由（风险面，非行数）：
  1. **冻结面命中**：需向 `Client/protocol.h` 与 `Server/protocol.h` 追加 PDU 枚举（manifest `frozen_paths`）→ 按 v1.7 规则自动升 LARGE，且改动须主人批准。
  2. **公共契约变更**：上传初始化协议（新 PDU + 新字段布局），另加 `upload_session` 表加列 + schema 版本提升。
  3. **跨会话**：客户端（摘要预计算/新分派）+ 服务端（预检查/命中事务/复算对照）+ DB 迁移，单会话放不下。
  4. **热路径**：改动上传状态机入口（新增命中分支），并新增服务端复算对照——登记已有对象所有权，回归面覆盖上传/下载/续传/分享/路径防护。
- 需求文档 `project-docs/requirements/REQ-EXT-FILE-002.md` 自声明 LARGE；本记录与之一致，不再改档。
- 流程版本：Development OS v1.7。分支：`codex/req-ext-file-002-fast-upload`，基线 `master@1123a98`。
- 节点计划：goal（本记录）→ design（`design.md` 随本记录落分支）→ **主人批准契约变更** → development → independent test（新会话）→ repair（如需）→ independent audit（新会话、只读）→ integration（三事实行）→ merge no-ff。
- `owner_agent: claude-code`（分支名沿用需求文档给定前缀 `codex/`，owner 以本行为准）。

## 依赖核查（2026-10-01 实测，非文档转述）
| 依赖 | 状态 |
|---|---|
| REQ-EXT-SEC-001 | ✅ merged；连接级 actor（`m_actorName`）、`pathInActorRoot`、`logicPathInActorRoot`、`isPublicRequestType` 门可用 |
| REQ-EXT-FILE-001 | ✅ merged `master@9faae6c`；四表 + `schema_version` + `FileObjectStore`（`hashStaging`/`commitUpload`/`attachEntry`/`releaseEntry`/`normalizeParent`/`logicPathInActorRoot`）+ `upload_session` CRUD + `migrateLegacyFiles` 全部在主干 |
| REQ-EXT-GROUP-002 | ✅ merged `master@ffaaacf`，本分支基线 `1123a98` 已含 |
| 并行冲突面 | 无活动开发区；现存分支均为已合并遗留（`codex/req-ext-group-001|002`、`small/*`）。本分支为唯一活动开发区 |

**复用盘点（代码事实，master@1123a98）** — 下列能力已存在，本需求只做接线与补强，不重写：
- `Server/migrate_ext_file_001.sql:41` `upload_session` 已含 `client_sha256 CHAR(64) NULL`（注释已声明"仅预检元数据，不作转正依据"）——秒传预检查的落点已备。
- `file_object.uk_sha256` **唯一键已存在**（`migrate_ext_file_001.sql:23`）——物理对象唯一性由 DB 保证，非逻辑约定。
- `FileObjectStore::commitUpload`（`Server/fileobjectstore.cpp:191`）已实现：对象 `FOR UPDATE` 命中 → 同名安置（`(1)` 递推）→ 条目建/复活 → 引用计数，**同一事务**；命中既有对象时丢弃 staging，物理只留一份。
- `ResumableUploadSession::beginStaged` 确定性 staging 路径（断点续传基础）；`OperateDB::{createUploadSession,updateUploadOffset,finishUploadSession}` 已就位。
- 缺口（本需求要补）：① 客户端从不计算摘要（`Server/msghandler.cpp:1162` 注释"客户端不提交摘要"）；② `commitUpload` 只比对**大小**，不比对客户端声明摘要；③ 无 PDU 层预检查入口；④ 无请求级幂等键。

## 契约影响（须主人批准）
| 面 | 变更 | 性质 |
|---|---|---|
| `Client/protocol.h` + `Server/protocol.h`（frozen） | 追加 `ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_REQUEST=97` / `_RESPOND=98`（现 max=96，**只追加不重编号**）+ 新字段偏移/magic 常量 | 已批准变更，见批准卡 |
| `upload_session` 表 | `ADD COLUMN client_request_id VARCHAR(64) NULL`、`ADD COLUMN file_id BIGINT NULL`、`ADD UNIQUE KEY uk_req (owner_name, client_request_id)` | 附加式（additive），NULL 行不冲突 |
| `schema_version` | 新增 `2026100101`，并入启动 `required` 计算 | 低版本启动 fail-fast（沿用 T1 语义） |
| 既有 PDU 编号 / 分块协议语义 | **零改动**（旧 `UPLOADFILE_INIT` 路径保留可用） | 回归面零破坏 |

## FILE-001 遗留事项核对（逐条定性，不整包接收）
| # | FILE-001 移交项 | 是否在本分支触碰面 | 处置 |
|---|---|---|---|
| L1 | `/filesys/<owner>/…/` 旧行被 `isLogicKeyForm` 判为 canonical → 静默保留、数据对查询不可见（判别盲区） | **是**（秒传目标目录解析必经 `normalizeParent`；`upload_session.parent_path` 为新写入域） | **本分支收口**：判别收紧为"不含 `:` 且首段不等于 fs 根 basename" |
| L2 | 带盘符 + 尾部斜杠旧行的告警泄漏（FILE-001 明文指派 FILE-002 收紧） | **是**（同一函数、同一列） | **本分支收口**：与 L1 同一处收紧，补夹具 |
| L3 | 路径 >191 字符旧文件记 `failed` 不入映射 | 否（存量迁移域） | 保持台账，不扩范围 |
| L4 | 未建目录条目的旧目录改名/移动后仅前缀级联 | 否（存量迁移域） | 保持台账；验收 5 回归时留意 |
| L5 | 隐藏文件不入迁移扫描 | 否 | 保持台账 |
| L6 | `claim-check` 缺 `manifest.yaml`；`validate-handoff.py` NODES 无 `independent-test` | 工具缺口，非产品 | 不属本分支；测试节点沿用 `node: test`，并在测试报告注明 |
| L7 | 32 字节展示名 + `(1)` 后缀溢出 | 否（`clampUtf8`/`splitName` 已实现） | 列为验证项，不重做 |
| L8 | F-6 并行节点错峰（共享 5000 端口 / MySQL / 整机 taskkill） | 环境纪律 | 本分支遵守：同一时刻单一活动开发区 |
| L9 | session-001 的 E7a 台账 | 他人需求面 | 不属本分支 |

> 说明：L1/L2 是同一函数的同一处判别缺陷，属"本需求自身触碰面内的既有缺陷"，非无关缺陷——按 v1.7「无关缺陷另开 SMALL 分支」不适用；若设计阶段实测证明其修复超出该函数局部（如需改 SQL 谓词语义），则降级为独立 SMALL 分支并记入台账。

## 门次与事件（append-only）
- 2026-10-01 goal：定档 LARGE，依赖四项实测通过，FILE-001 遗留 L1/L2 纳入本分支，契约变更批准卡提交主人（待答）。

## 下一节点
design 已成文（`design.md`）→ 等主人对契约变更批准卡作答 → 批准后进 development。
