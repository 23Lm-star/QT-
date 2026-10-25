// WebSocket <-> TCP 透明桥接：不解析 PDU，纯字节双向转发。
// 关键约束：
//  - 每条 WS 连接独占一条到上游 Server 的 TCP 连接（认证态绑定在连接内存，不可池化）。
//  - WS 二进制消息天然保界；浏览器每次 send 一个完整 PDU，网关直接写入 TCP。
//  - TCP->WS 方向按 TCP 读到的原始块转发，边界任意；浏览器端按 uiTotalLen 重新组包。
//  - 背压：下载时服务端流式快推，用 ws.bufferedAmount 高/低水位暂停/恢复 TCP 读取。
const net = require('net');
const { WebSocketServer } = require('ws');

// WS 发送缓冲高/低水位（字节）。
const WS_HIGH_WATER = 4 * 1024 * 1024;
const WS_LOW_WATER = 1 * 1024 * 1024;
// 保活探测周期：30s，覆盖 Cloudflare Tunnel 约 100s 空闲超时。
const KEEPALIVE_MS = 30000;

function attachWebSocketServer(httpServer, config, log = console) {
  const wss = new WebSocketServer({
    server: httpServer,
    path: '/ws',
    maxPayload: config.maxPayload,
    // 二进制协议，关闭压缩：既省 CPU 又避免压缩边界干扰字节透传。
    perMessageDeflate: false,
  });

  let activeConnections = 0;

  wss.on('connection', (ws, req) => {
    const remote = `${req.socket.remoteAddress}:${req.socket.remotePort}`;

    if (activeConnections >= config.maxConnections) {
      ws.close(4003, 'gateway at capacity');
      return;
    }
    activeConnections++;

    // 保活标记：pong 回来置真；探测周期内未回则 terminate。
    ws.isAlive = true;
    ws.on('pong', () => { ws.isAlive = true; });

    let closed = false;
    const tcp = net.connect(config.serverPort, config.serverHost);
    tcp.setNoDelay(true);

    let tcpEstablished = false;
    let tcpPaused = false;
    // TCP 连通前浏览器已发来的字节，暂存后按序补写。
    const pending = [];

    function cleanup(code, reason) {
      if (closed) return;
      closed = true;
      activeConnections--;
      try { tcp.destroy(); } catch { /* ignore */ }
      try {
        if (ws.readyState === ws.OPEN || ws.readyState === ws.CONNECTING) {
          ws.close(code, reason);
        }
      } catch { /* ignore */ }
    }

    tcp.on('connect', () => {
      tcpEstablished = true;
      for (const buf of pending) tcp.write(buf);
      pending.length = 0;
    });

    tcp.on('error', (err) => {
      log.error(`[bridge] tcp error ${remote}: ${err.message}`);
      cleanup(4001, 'upstream connect failed');
    });

    // 上游关闭 TCP（被踢下线/服务端重启）：正常关闭对应 WS。
    tcp.on('close', () => cleanup(1000, 'upstream closed'));

    // TCP -> WS，带背压。
    tcp.on('data', (chunk) => {
      if (closed || ws.readyState !== ws.OPEN) return;
      ws.send(chunk, { binary: true }, (err) => {
        if (err) { cleanup(1011, 'ws send error'); return; }
        if (tcpPaused && ws.bufferedAmount < WS_LOW_WATER) {
          tcpPaused = false;
          tcp.resume();
        }
      });
      if (!tcpPaused && ws.bufferedAmount > WS_HIGH_WATER) {
        tcpPaused = true;
        tcp.pause();
      }
    });

    // WS -> TCP，纯字节写入。文本帧不属协议，忽略。
    ws.on('message', (data, isBinary) => {
      if (closed) return;
      if (!isBinary) return;
      const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
      if (!tcpEstablished) { pending.push(buf); return; }
      tcp.write(buf);
    });

    ws.on('error', (err) => {
      log.error(`[bridge] ws error ${remote}: ${err.message}`);
      cleanup(1011, 'ws error');
    });

    ws.on('close', () => cleanup(1000, 'ws closed'));
  });

  // 全局保活探测：一个定时器覆盖所有连接。
  const interval = setInterval(() => {
    for (const ws of wss.clients) {
      if (ws.isAlive === false) { ws.terminate(); continue; }
      ws.isAlive = false;
      try { ws.ping(); } catch { /* ignore */ }
    }
  }, KEEPALIVE_MS);
  wss.on('close', () => clearInterval(interval));

  return wss;
}

module.exports = { attachWebSocketServer };
