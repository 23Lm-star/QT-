// 网关配置：全部可用环境变量覆盖，默认值对应本机开发（Server.exe 监听 127.0.0.1:5000）。
const path = require('path');

module.exports = {
  // 网关自身 HTTP/WS 监听地址与端口。0.0.0.0 便于内网穿透工具接入。
  wsHost: process.env.GATEWAY_HOST || '0.0.0.0',
  wsPort: parseInt(process.env.GATEWAY_PORT || '8080', 10),

  // 上游 Qt 服务端 TCP 地址（认证态是连接级内存状态，网关每条 WS 独占一条 TCP，不复用）。
  serverHost: process.env.GATEWAY_TARGET_HOST || '127.0.0.1',
  serverPort: parseInt(process.env.GATEWAY_TARGET_PORT || '5000', 10),

  // 并发 WS 连接上限，超出以 4003 关闭。
  maxConnections: parseInt(process.env.GATEWAY_MAX_CONN || '200', 10),

  // 单个 WS 消息上限：上传数据块正文最大 1MiB + 76B 头 + 余量，取 2MiB 足够。
  maxPayload: parseInt(process.env.GATEWAY_MAX_PAYLOAD || String(2 * 1024 * 1024), 10),

  // 前端构建产物目录（生产由网关同源托管；开发用 Vite dev server 反代 /ws）。
  staticDir: process.env.GATEWAY_STATIC_DIR
    ? path.resolve(process.env.GATEWAY_STATIC_DIR)
    : path.join(__dirname, '..', 'frontend', 'dist'),
};
