# REQ-EXT-TRANSFER-001 多文件并行传输（有限并发）— 任务记录

## 档位
- **LARGE**（goal 定档 2026-10-01）。理由（风险面，非行数）：
  1. **冻结面命中**：需向 `Client/protocol.h` 与 `Server/protocol.h` 追加 transfer_id 字段（按 PDU 落位）与拟新增 ABORT PDU（manifest `frozen_paths`）→ 按 v1.7 规则自动升 LARGE，改动须主人批准。
  2. **公共契约变更**：上传/下载全族 PDU（29/30/31/32、40/41/42/43、101/102、105/106）扩展 + 拟追加 107/108；`0` 值保留 = 旧单流路径（旧编号语义零改动）。
  3. **跨会话**：客户端（单例 → per-transfer 多实例、UI 任务列表、暂停/取消）+ 服务端（单会话 → QHash 状态表、per-transfer 写盘在途位、下载 round-robin 调度、空闲回收），单会话放不下。
  4. **热路径**：上传/下载状态机核心改造，回归覆盖全部六套 e2e 套件（cross_tree/entry_ops/dedup/sec001/fastupload/groupfile）。
- 需求文档 `project-docs/requirements/REQ-EXT-TRANSFER-001-multi-file-parallel-transfer.md` 自声明 LARGE；本记录与之一致，不再改档。
- 流程版本：Development OS v1.7。分支：`feat/req-ext-transfer-001-parallel-transfer`，基线 `master@53b048b`。
- 节点计划：goal（本记录）→ design（逐字节协议设计 + 状态机 + 调度算法 + 测试矩阵 → `design.md`）→ **主人批准契约变更** → development → independent test（新会话）→ repair（按需）→ independent audit（新会话、只读）→ integration（三事实行）→ merge no-ff。
- `owner_agent: claude-code`。

## 依赖核查（2026-10-01 实测，非文档转述）
| 依赖 | 状态 |
|---|---|
| REQ-EXT-SEC-001 | ✅ merged `7fc47c1`；连接级 actor 门（`m_actorName`、`pathInActorRoot`） |
| REQ-EXT-GROUP-001 | ✅ merged `ee75441`；群成员资格（101 判定复用面） |
| REQ-EXT-GROUP-002 | ✅ merged `ffaaacf`；群消息 PDU 88–96 |
| REQ-EXT-FILE-001 | ✅ merged `9faae6c`；对象仓 + 五表 + `upload_session` CRUD + 逻辑键 |
| REQ-EXT-FILE-002 | ✅ merged `abe2e5d`；秒传 97/98（本需求保持单飞不改造） |
| REQ-EXT-GROUP-003 | ✅ merged `495e552`；群文件 PDU 99–106 + `group_file` 表（群上传/下载 + 续传 `nextOffset` 现成） |
| 并行冲突面 | 无活动开发区；master@53b048b 为唯一基线 |
| 回归运行形态 | 已统一（`0d92607` + `53b048b`）；六套件按 E2E-RUNFORM 规格跑 |

**复用盘点（代码事实）** — 并行化的关键既有设施：
- 上传续传：个人 29/30 `resumeOffset`、群 101/102 `nextOffset`（`resumeEnabled=true`）**均已具备** → 暂停/恢复走既有续传路径，零新协议（goal 取证）。
- 背压：入站 `MAX_INBOUND_PDU_SIZE=16KB` + `MAX_PENDING_INBOUND_BYTES=256KB`；出站 256KB 高水位节流（Server/protocol.h:193-194）。
- 清扫定时器模式：`MyTcpServer` 僵尸连接清扫（空闲回收可参照）。
- 上传 DATA 已有 offset 连续性/长度边界/magic 校验（并行后逐 transfer 化，语义不变）。

## 用户决策（2026-10-01，AskUserQuestion 三项确认）
1. 传输路径 = **全路径并行**：个人上传、个人下载、群上传、群下载（下载数据通道 42/43 本就共用，统一加编号优于分路径特判）。
2. 单连接并发上限 = **3**（常量，可调；超限排队不报错）。
3. UI = 多选选择器 + 文件级任务列表 + **每个上传任务可手动暂停/恢复/取消**（用户追加，扩展原范围）。
- 明确非目标：单文件乱序分片、动态分片、任务持久化、跨连接并行、限速/配额、下载任务暂停/取消（下载仅多选并行）。

## 契约影响（须主人批准；批准卡随 design.md 提交）
| 面 | 变更 | 性质 |
|---|---|---|
| `Client/protocol.h` + `Server/protocol.h`（frozen） | 29/30/31/32、40/41/42/43、101/102、105/106 追加 transfer_id（u64，落位草案见需求文档 §8.2）；拟新增 `ENUM_MSG_TYPE_UPLOADFILE_ABORT_REQUEST/RESPOND=107/108`（形态 design 裁定） | 只追加不重编号；`0` 值保留 = 旧单流路径 |
| 既有 PDU 编号 / 分块协议语义 | **零改动**（id=0 旧路径隔离；97/98 不加 id） | 旧客户端与全部既有 e2e 脚本零改动通过 |
| DB schema | **零变更**、无迁移 | 附加值 |

## 门次与事件（append-only）
- 2026-10-01 design：`design.md` 落盘（逐字节落位表 / 107-108 形态 / 两端状态机 / ring 调度 / 60s 回收 / 测试矩阵 18 例 / 兼容不变式 I1-I6 / 冻结面逐字批准稿 §9）。8 项设计裁决 D1-D8 记录在案。契约变更批准卡以业务语言提交，**主人批准：同意**；并发名额 Q1 裁决为**上传 3 + 下载 3（相互独立）**，已回写 `design_status: CONFIRMED`。
- 2026-10-01 goal：定档 LARGE。方案 B（协议级多路复用）裁决 = **采用，带 6 处修正**（M1 transfer_id 覆盖初始化握手 = 硬缺陷修正；M2 0 值保留 = 免费回归护栏；M3 服务端 per-transfer 状态 + 上限；M4 下载 round-robin 公平调度；M5 SHA-256 移出 UI 线程；M6 文件级任务列表 UI）。排除：多连接并行（与单登录版本模型冲突）、纯客户端串行队列（不产生并行）。需求文档落盘，分支自 `master@53b048b` 切出。契约变更批准卡待 design 节点提交。

## 下一节点
development（新会话，owner 会话）：按 `design.md` §9 逐字批准稿写入两端 `protocol.h`，随后 T2–T8（服务端 per-transfer 状态表 + ring 调度 + 107/108 + 上限/回收；客户端 TransferManager + 路由改造 + 任务列表 UI + SHA-256 后台线程；新 e2e 套件；六套件回归 + 文档同步）。退出条件：`pipeline/build.ps1` / `test.ps1` / `audit.ps1` 全绿 + 六套件 + `parallel_e2e.py` 全绿 → 交 independent test（新会话）。
