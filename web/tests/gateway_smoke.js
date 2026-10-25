// 网关透明性冒烟测试（无需真实 Server.exe）：
// 起一个 TCP echo 服务，网关指向它，WS 客户端发送若干二进制帧，断言原样回显。
// 运行：node web/tests/gateway_smoke.js
const net = require('net');
const http = require('http');
const path = require('path');
const assert = require('assert');

const GATEWAY_DIR = path.join(__dirname, '..', 'gateway');
const { attachWebSocketServer } = require(path.join(GATEWAY_DIR, 'bridge.js'));
const WebSocket = require(path.join(GATEWAY_DIR, 'node_modules', 'ws'));

const ECHO_PORT = 5599;
const GW_PORT = 8098;

function startEchoServer() {
  return new Promise((resolve) => {
    const srv = net.createServer((sock) => {
      sock.on('data', (d) => sock.write(d)); // 原样回显
      sock.on('error', () => {});
    });
    srv.listen(ECHO_PORT, '127.0.0.1', () => resolve(srv));
  });
}

async function main() {
  const echo = await startEchoServer();
  const config = {
    serverHost: '127.0.0.1',
    serverPort: ECHO_PORT,
    maxConnections: 10,
    maxPayload: 2 * 1024 * 1024,
  };
  const httpServer = http.createServer((req, res) => res.writeHead(200).end('ok'));
  attachWebSocketServer(httpServer, config, { error: () => {}, log: () => {} });
  await new Promise((r) => httpServer.listen(GW_PORT, '127.0.0.1', r));

  const ws = new WebSocket(`ws://127.0.0.1:${GW_PORT}/ws`);
  const received = [];
  let totalExpected = 0;

  // 构造三种载荷：小帧、恰好 64KiB、约 700KiB（跨多个 TCP 段，验证背压/组包不丢字节）。
  const payloads = [
    Buffer.from([1, 2, 3, 4, 5]),
    Buffer.alloc(64 * 1024, 0xab),
    Buffer.alloc(700 * 1024, 0xcd),
  ];
  totalExpected = payloads.reduce((s, b) => s + b.length, 0);

  await new Promise((resolve, reject) => {
    ws.on('open', () => {
      for (const p of payloads) ws.send(p);
    });
    ws.on('message', (data) => {
      received.push(Buffer.from(data));
      const got = received.reduce((s, b) => s + b.length, 0);
      if (got >= totalExpected) resolve();
    });
    ws.on('error', reject);
    setTimeout(() => reject(new Error('timeout waiting echo')), 5000).unref?.();
  });

  const echoed = Buffer.concat(received);
  const sent = Buffer.concat(payloads);
  assert.strictEqual(echoed.length, sent.length, `长度不符: ${echoed.length} != ${sent.length}`);
  assert.ok(echoed.equals(sent), '字节内容不符（透传被破坏）');

  console.log(`[smoke] OK 透传 ${echoed.length} 字节，逐字节一致（含 ${payloads.length} 个分帧/大块）`);

  ws.close();
  httpServer.close();
  echo.close();
  process.exit(0);
}

main().catch((e) => { console.error('[smoke] FAIL', e.message); process.exit(1); });
