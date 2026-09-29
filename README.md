# QT 云盘聊天系统

基于 Qt 5 与自研 TCP 二进制协议的桌面云盘 + 聊天应用（C/S 架构）。

> 学习项目说明：本项目为个人学习/课程实践项目，非生产环境代码，
> 未做完整的安全加固与性能压测。请勿直接用于生产。

## 功能概览

- 用户注册 / 登录（含验证码、冷却与锁定）
- 账号注销（删除账号）
- 好友管理（查找、添加、删除、刷新在线列表）
- 私聊消息（含历史消息拉取）
- 文件云盘：目录管理、上传 / 下载、断点续传、重命名、移动、删除、分享（好友协作）
- 外链分享链接（创建 / 解析 / 查询）

## 技术栈

- C++11 / Qt 5.14.2（MinGW 7.3.0 64bit）
- 自研 PDU 二进制协议（TCP）
- MySQL（服务端持久化，RAII 数据库连接池）
- 自研对象池 / 内存分配组件（`Server/ObjectPool.h`、`Server/memalloc.h`）

## 目录结构

| 目录 | 说明 |
|---|---|
| `Client/` | 桌面客户端（Qt Widgets） |
| `Server/` | TCP 服务端（业务处理、数据库、文件沙箱） |
| `project-docs/` | 治理记录、架构与业务文档、任务与证据归档 |
| `pipeline/` | 构建 / 测试 / 审计 / 状态四脚本 |
| `build/` | 构建产物与运行时证据（不入版本库） |

## 构建

### 依赖

- Qt 5.14.2 mingw73_64（`C:\Qt\5.14.2\mingw73_64`）
- MinGW 7.3.0（`C:\Qt\Tools\mingw730_64`）
- MySQL 服务端

### 配置

1. 复制 `Server/connect.config.template` → `Server/connect.config`，填入真实数据库信息
2. 复制 `Client/connect.config.template` → `Client/connect.config`，填入服务端地址

> `connect.config` 已被 `.gitignore` 排除，切勿提交真实凭据。

### 构建

```powershell
powershell -File pipeline/build.ps1
```

产物位于 `build/build-Server/` 与 `build/build-Client/`。

## 运行

1. 初始化数据库：对 MySQL 执行 `project-docs/init_test_data.sql`（建表 + 样例数据）
2. 启动服务端（`build/build-Server/` 下的可执行文件）
3. 启动客户端（`build/build-Client/` 下的可执行文件）

## 文档

- 架构：`project-docs/architecture.md`
- 模块：`project-docs/modules.md`
- 业务功能：`project-docs/业务功能文档.md`
- 缺陷与修复记录：`project-docs/defects/`、`project-docs/fix-specs/`

## 许可证

见 [LICENSE](LICENSE)（MIT）。
