// 网关入口：单端口同源提供 静态前端托管 + WebSocket(/ws) 透明桥接。
// 生产：浏览器连 ws(s)://host/ws -> 网关 -> TCP 上游 Qt 服务端；页面由本进程托管。
// 开发：Vite dev server 反代 /ws 到本进程，staticDir 可指向未构建目录（会回退提示）。
const http = require('http');
const fs = require('fs');
const path = require('path');
const config = require('./config');
const { attachWebSocketServer } = require('./bridge');

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.ico': 'image/x-icon',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.ttf': 'font/ttf',
  '.map': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
};

const NOT_BUILT_MSG =
  'gateway: 前端未构建（web/frontend/dist 缺失）。\n' +
  '开发模式请用 Vite dev server；或先构建：npm --prefix web/frontend install && npm --prefix web/frontend run build';

function sendFile(res, req, filePath, fallback) {
  fs.stat(filePath, (err, stat) => {
    if (err || !stat.isFile()) {
      if (fallback) { sendFile(res, req, path.join(config.staticDir, 'index.html'), false); return; }
      res.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
      res.end(NOT_BUILT_MSG);
      return;
    }
    const ext = path.extname(filePath).toLowerCase();
    const headers = {
      'content-type': MIME[ext] || 'application/octet-stream',
      'content-length': stat.size,
    };
    // index.html 不缓存，带 hash 的构建产物长缓存。
    if (ext === '.html') headers['cache-control'] = 'no-cache';
    res.writeHead(200, headers);
    if (req.method === 'HEAD') { res.end(); return; }
    fs.createReadStream(filePath).pipe(res);
  });
}

function serveStatic(req, res) {
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    res.writeHead(405, { 'content-type': 'text/plain; charset=utf-8' });
    res.end('method not allowed');
    return;
  }
  let urlPath;
  try {
    urlPath = decodeURIComponent(req.url.split('?')[0]);
  } catch {
    res.writeHead(400).end('bad request');
    return;
  }
  if (urlPath === '/') urlPath = '/index.html';
  // 规范化并阻断 ../ 目录穿越；最终必须仍位于 staticDir 内。
  const rel = path.normalize(urlPath).replace(/^([/\\])+/, '');
  const filePath = path.join(config.staticDir, rel);
  // REQ-AUDIT-FULL-001：改用 relative 判定，避免同前缀兄弟目录（如 dist-private）误放行
  if (path.relative(config.staticDir, filePath).startsWith('..')) {
    res.writeHead(403, { 'content-type': 'text/plain; charset=utf-8' });
    res.end('forbidden');
    return;
  }
  // SPA 回退：未命中的路径交给 index.html（Vue 使用 hash 路由时通常不触发）。
  sendFile(res, req, filePath, true);
}

const server = http.createServer(serveStatic);
attachWebSocketServer(server, config);

server.listen(config.wsPort, config.wsHost, () => {
  console.log(`[gateway] HTTP/WS listening on http://${config.wsHost}:${config.wsPort}`);
  console.log(`[gateway]   WebSocket path: /ws`);
  console.log(`[gateway]   upstream TCP  : ${config.serverHost}:${config.serverPort}`);
  console.log(`[gateway]   static dir    : ${config.staticDir}`);
});

// 优雅退出：关闭监听与所有连接。
for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    console.log(`[gateway] ${sig} received, shutting down`);
    server.close(() => process.exit(0));
    setTimeout(() => process.exit(0), 2000).unref();
  });
}
