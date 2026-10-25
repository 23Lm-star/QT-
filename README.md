# QT 云盘聊天系统

基于 Qt Widgets（桌面界面）、C++11 和自定义 TCP 二进制协议的客户端/服务端学习项目，提供云盘文件管理、好友、私聊、群聊及分享。当前是本地开发版本，未做生产部署验收。

## 文档入口

- [PRD.md（业务需求与验收结论）](PRD.md)：业务边界、已有能力和待完成正式需求。
- [TODO.md（待处理缺陷与想法）](TODO.md)：缺陷、待复核事项、未排期扩展及已接受风险。
- [AGENTS.md（协作规则）](AGENTS.md)：四文件读取方式、开发、验证、审查及归档规则。

2026-10-24 已由 Development OS v1.7（旧开发治理系统）迁移到四文件工作流。旧材料在 [历史目录](docs/archive/legacy-development-os-2026-10-24/)，默认不加载；旧管线不再是启动、构建或验收门槛。迁移只改变文档和协作规则。

## 当前快照

- 当前代码可定位到：注册/登录/注销、验证码与冷却、密码加盐及旧账号升级、唯一有效登录；好友与私聊；群生命周期、群文本、阅读游标与群文件；逻辑文件资产、秒传、去重、引用回收；多文件有限并发、上传下载暂停/继续/取消、传输快照；应用内分享链接创建/解析/查询/续期/重新生成。
- 上述是代码和历史集成记录所支持的能力范围，**不是本轮验收通过声明**。基线业务规则与依据见 [PRD 当前能力](PRD.md#当前能力与业务基线)。
- 接手时主分支为 `master`，提交短标识 `ebabb0c`，相对本地记录的远端领先 12 个提交；工作区已有 81 个修改文件和 3 个删除文件。这些是迁移前状态，未进行提交、清理或业务修改。
- 本轮验证仅覆盖治理迁移、文档链接、遗留事项登记和文件完整性。未执行构建、数据库测试或图形界面验收，不能沿用旧报告宣布当前业务通过。
- 下一步优先核对 [连接失败体验](TODO.md#产品缺陷与历史疑点)、[测试稳定性](TODO.md#测试与验证缺口)，正式未完成需求见 PRD。业务实施顺序尚未重新排期。

## 环境与配置

| 项目 | 当前约束 |
|---|---|
| 平台与编译 | Windows；Qt 5.14.2 `mingw73_64`；MinGW 7.3.0 64 位；C++11 |
| Qt 安装 | `C:/Qt/5.14.2/mingw73_64`，使用该目录的 qmake，避免误用其他软件附带版本 |
| 编译器 | `C:/Qt/Tools/mingw730_64` |
| 数据库 | MySQL；Qt QMYSQL（MySQL 驱动）及位数匹配的客户端 DLL；数据库名以本机配置为准，测试历史使用 `mydb` |
| 测试运行时 | `py -3.13 -X utf8`；部分套件还需要 MinGW g++ |
| 运行目录 | Release（发布配置）程序位于 `dist/`，服务端和客户端均从该目录启动 |

先复制 `Server/connect.config.template` 为 `Server/connect.config`，填写监听地址、端口、文件根、数据库主机、端口、库名、用户、密码，共 8 行。复制 `Client/connect.config.template` 为 `Client/connect.config`，填写服务端地址、端口、本地文件根，共 3 行。

当前两端从 `:/connect.config`（Qt 编译资源）读取，按 CRLF（回车换行）分行，**不能直接保存成 LF**；修改后必须重新构建。格式校验不足是现有限制，外置和校验需求见 [REQ-CFG-010](PRD.md#req-cfg-010配置外置与明确报错)。敏感配置继续由 Git 忽略。客户端文件根与服务端存储根应分开，防止本机下载与源文件重名。

## 构建、部署与启动

最短构建路径：Qt Creator 分别打开 `Server/Server.pro` 和 `Client/Client.pro`，选择 Desktop Qt 5.14.2 MinGW 64-bit 套件，构建 Release。两个 `.pro` 已配置 Release 输出到 `dist/`；Debug（调试配置）产物保留在构建目录。

以下 PowerShell 命令从项目根运行，使用独立构建子目录；本次只核对了命令和 `.pro` 依据，未实际编译：

```powershell
$projectRoot = (Get-Location).Path # 项目根目录
$env:Path = 'C:\Qt\5.14.2\mingw73_64\bin;C:\Qt\Tools\mingw730_64\bin;C:\Qt\Tools\mingw730_64\libexec\gcc\x86_64-w64-mingw32\7.3.0;' + $env:Path
foreach ($projectName in @('Server', 'Client')) { # 项目名称，先服务端后客户端
    $buildPath = Join-Path $projectRoot "build/four-file-$projectName-release" # 独立构建目录
    New-Item -ItemType Directory -Force $buildPath | Out-Null
    Push-Location $buildPath
    try {
        & 'C:/Qt/5.14.2/mingw73_64/bin/qmake.exe' (Join-Path $projectRoot "$projectName/$projectName.pro") 'CONFIG+=release' 'CONFIG-=debug'
        if ($LASTEXITCODE -ne 0) { throw 'qmake 失败，停止构建' }
        & 'C:/Qt/Tools/mingw730_64/bin/mingw32-make.exe' -j4
        if ($LASTEXITCODE -ne 0) { throw '编译失败，停止构建' }
    } finally { Pop-Location }
}
```

部署前关闭本项目正在运行的程序，避免 Windows 文件锁。Qt 运行库使用同一 Qt 安装中的 `windeployqt.exe --release dist/Server.exe` 与 `windeployqt.exe --release dist/Client.exe` 部署；另核对 QMYSQL 驱动和匹配的 MySQL 客户端 DLL。不要从旧构建目录覆盖 `dist` 的程序或 DLL。旧 `artifacts.ps1 -Deploy`（产物部署脚本）已退役，其历史缺陷见 TODO。

初始化数据库仅用于新建的隔离开发/测试库：`project-docs/init_test_data.sql` 提供 `user_info`、`login_record`、`friend` 及测试数据；扩展结构使用 `Server/migrate_*.sql`、`Server/share_link.sql` 等既有脚本。执行前核对目标库、已应用迁移和对应回滚，不能把所有迁移及回滚批量执行。已有真实数据的数据库不重灌测试种子。

分别在两个终端执行：

```powershell
Set-Location E:/qt/xiangmu/dist
.\Server.exe
```

```powershell
Set-Location E:/qt/xiangmu/dist
.\Client.exe
```

最短体验：服务端正常监听 → 客户端注册并登录 → 上传并下载文件 → 两个账号互加好友并发送私聊 → 创建群并邀请另一账号 → 群文本/群文件 → 创建、领取和续期应用内分享链接。账号注销会删除账户，不作为初次体验的必要步骤。

### 网页端（REQ-WEB-001，进行中）

浏览器网页端与桌面端共用同一 `Server.exe`、同一账号体系，经 WebSocket↔TCP 透明网关接入，**服务端与桌面客户端零改动**。当前进度：M1 登录（注册/登录/验证码/心跳）与 M2 好友私聊（好友/在线用户/申请-同意/实时私聊/离线补发）已通过用户人工验收；M3 云盘传输（目录增删改移/上传下载/分片停等/秒传/会话内断点续传/暂停取消/传输面板）已通过用户人工验收（2026-10-25）；下载为全大小统一"增量 Blob 聚合、完成后一次性保存到浏览器下载目录"（暂停/取消/失败不落任何文件）；M4 群、M5 分享、M6 公网穿透未实现。方案、验证与限制见 [变更记录](docs/changes/REQ-WEB-001/记录.md)。

先启动服务端（见上），再分别启动网关与前端。网关默认连 `127.0.0.1:5000`、监听 `8080`：

```powershell
# 生产形态：构建前端并由网关同源托管（含 /ws）
npm --prefix web/frontend install
npm --prefix web/frontend run build
npm --prefix web/gateway install
npm --prefix web/gateway start        # 打开 http://127.0.0.1:8080/
```

```powershell
# 开发形态：Vite dev server 热更新，/ws 反代到网关
npm --prefix web/gateway start        # 网关 :8080
npm --prefix web/frontend run dev     # 打开 http://127.0.0.1:5173/
```

网关可用环境变量覆盖：`GATEWAY_PORT`、`GATEWAY_TARGET_HOST/PORT`、`GATEWAY_STATIC_DIR`。多端登录沿用唯一有效登录：同一账号桌面与网页会互踢。公网访问需经内网穿透（如 Cloudflare Tunnel）终止 TLS 后暴露网关端口——M6 里程碑处理，当前未验证。

## 验证入口

原登记的 17 个回归套件如下，入口文件原地保留。运行前完成构建和运行库部署，准备隔离 MySQL 测试库、配置与所需迁移，并确保 TCP 5000 没有无关服务。套件会创建/修改测试用户及文件，只能串行运行；不要在真实用户数据环境直接执行。

每条用 `py -3.13 -X utf8 <入口文件>` 运行，检查实际退出码、失败断言和最终汇总；失败即停止归因，不用后来一次通过覆盖前面的失败。旧登记文件随历史管线归档，本表是当前命令入口；本轮未运行这些业务测试。

| 验证范围 | 入口文件 |
|---|---|
| 会话身份与授权 | `project-docs/evidence/REQ-EXT-SEC-001/verify.py` |
| 跨用户授权回归 | `project-docs/evidence/REQ-EXT-FILE-001/sec001_regression.py` |
| 文件条目操作 | `project-docs/evidence/REQ-EXT-FILE-001/entry_ops_e2e.py` |
| 文件去重 | `project-docs/evidence/REQ-EXT-FILE-001/dedup_e2e.py` |
| 跨目录文件操作 | `project-docs/evidence/REQ-EXT-FILE-001/cross_tree_e2e.py` |
| 文件秒传 | `project-docs/evidence/REQ-EXT-FILE-002/fastupload_e2e.py` |
| 群文本与权限 | `project-docs/evidence/REQ-EXT-GROUP-002/verify.py` |
| 群文件 | `project-docs/evidence/REQ-EXT-GROUP-003/groupfile_e2e.py` |
| 群阅读游标 | `project-docs/evidence/REQ-EXT-GROUP-004/verify.py` |
| 文件逻辑路径与授权 | `project-docs/evidence/REQ-EXT-SEC-002/verify.py` |
| 唯一有效登录 | `project-docs/evidence/REQ-EXT-SESSION-001/unique_login_e2e.py` |
| 多任务并行传输 | `project-docs/evidence/REQ-EXT-TRANSFER-001/parallel_e2e.py` |
| 下载暂停/取消/继续 | `project-docs/evidence/REQ-EXT-TRANSFER-002/verify.py` |
| 上传连续性与吞吐 | `project-docs/evidence/REQ-EXT-TRANSFER-003/verify.py` |
| 分享链接生命周期 | `project-docs/evidence/REQ-EXT-SHARE-001/verify.py` |
| 注销后同名重注册登录 | `project-docs/evidence/REQ-EXT-SESSION-001/relogoff_login_e2e.py` |
| 启动期在线态对账 | `project-docs/evidence/REQ-EXT-SESSION-001/reconcile_online_e2e.py` |

另有 `project-docs/evidence/REQ-EXT-RECLAIM-001/reclaim_e2e.py`（物理对象与临时上传回收），历史未纳入常规登记，先处理 [D-022](TODO.md#测试与验证缺口)。上传会话无网络单元测试和稳定的统一运行入口见 [PRD](PRD.md#需求索引)。

人工验收重点：连接失败提示、重连后用户目录、下载分片丢失提示、客户端重启续传、群聊文字布局、白字背景可读性、分享失效后的三种操作。多数 Python 套件只驱动服务端，不能替代真实 `Client.exe` 的界面验收；群阅读游标另有头文件编译探针。

## 目录、数据流与限制

| 路径 | 职责 |
|---|---|
| `Client/` | Qt 桌面界面、协议收发、传输任务和本地快照 |
| `Server/` | TCP 会话与请求分发、鉴权、MySQL 访问、文件资产、回收及内存分配组件；含迁移与回滚 SQL |
| `web/` | REQ-WEB-001 网页端：`gateway/`（WebSocket↔TCP 透明网关，Node.js）+ `frontend/`（Vue 3 SPA，浏览器端实现 PDU 协议）+ `tests/`（网关/协议/登录 e2e） |
| `project-docs/evidence/` | 保持兼容的测试代码、辅助文件和本地原始输出；不是当前治理入口 |
| `project-docs/init_test_data.sql` | 数据库基础建表与测试种子 |
| `build/`、`build-*/`、`dist/` | 本机构建、部署及运行产物，继续忽略 |
| `docs/archive/legacy-development-os-2026-10-24/` | 旧治理、管线、设计、交接和报告，默认不读 |

数据流：界面操作 → 客户端协议封装 → TCP → 服务端会话鉴权和请求处理 → MySQL / 文件存储 → 应答、消息广播或传输确认 → 客户端更新。`file_object`（物理内容对象）与 `user_file`（用户逻辑条目）分离；群、分享和个人文件复用授权与文件资产边界。内存管理含线程缓存、中央缓存、页缓存与基数树；数据库连接池采用 RAII（作用域管理资源）方式。

兼容边界：两端协议编号保持一致，只允许追加，不重排旧编号。服务端在线态对账依赖单实例管理同一库；多实例共享库尚未支持。TLS（传输加密）、强密码派生、无密码会话恢复及动态/乱序分片等仍在 TODO，不因旧路线图提出过就视为已实现。

## 变更与许可证

2026-10-24：初始化四文件治理，迁移未完成需求、缺陷和想法，退出旧治理入口；保留业务源码、测试与运行必需数据。旧业务实现历史见 [历史交接目录](docs/archive/legacy-development-os-2026-10-24/project-docs/handoffs/)，只用于追溯。

MIT，见 [LICENSE](LICENSE)。
