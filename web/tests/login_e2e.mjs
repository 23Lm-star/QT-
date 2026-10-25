// M1 端到端登录验证：复用前端真实协议代码（account.js + NetClient），
// 经 WebSocket 网关对真实 Server.exe 跑完整账号流程，并与直连 TCP 对比证明透明性。
// 前置：dist/Server.exe 已监听 5000，网关已监听 8080（由编排脚本负责）。
// 运行：node web/tests/login_e2e.mjs
import assert from 'node:assert';
import net from 'node:net';

const SRC = '../frontend/src/protocol';
const { NetClient } = await import(`${SRC}/client.js`);
const { setNet } = await import(`${SRC}/net.js`);
const account = await import(`${SRC}/commands/account.js`);
const { decodePDU, encodePDU } = await import(`${SRC}/pdu.js`);
const { Framer } = await import(`${SRC}/framer.js`);
const { T } = await import(`${SRC}/msgtypes.js`);
const { writeFixedStringNul, writeExactBytes, concatBytes, readI32, readFixedString, textEncoder } = await import(`${SRC}/fields.js`);

const GW_URL = process.env.GW_URL || 'ws://127.0.0.1:8080/ws';
const TCP_HOST = '127.0.0.1';
const TCP_PORT = parseInt(process.env.TCP_PORT || '5000', 10);
const PASSWORD = '123456';
const CLIENT_ID = 'c0ffeec0ffeec0ffeec0ffeec0ffee00'; // 32 hex
const USER = process.env.E2E_USER || ('webt_' + Math.floor(Date.now() / 1000)); // 唯一测试用户（编排脚本负责清理）

let passCount = 0;
const failures = [];
function check(id, desc, cond, detail) {
  if (cond) { passCount++; console.log(`[PASS] ${id} ${desc} :: ${detail}`); }
  else { failures.push(id); console.log(`[FAIL] ${id} ${desc} :: ${detail}`); }
}

// 直连 TCP 会话：暴露与 NetClient 相同的 request(reqType,respType,caData,msg) 契约，
// 使 account.js 可无差别复用于两种传输，直接对比网关 vs 直连的协议结果。
function connectRawTcp() {
  return new Promise((resolve, reject) => {
    const sock = net.connect(TCP_PORT, TCP_HOST);
    const framer = new Framer();
    const waiters = [];
    const api = {
      request(reqType, respType, caData = null, msg = null, timeout = 5000) {
        return new Promise((res, rej) => {
          const w = { type: respType };
          w.timer = setTimeout(() => {
            const i = waiters.indexOf(w); if (i >= 0) waiters.splice(i, 1);
            rej(new Error(`tcp 请求超时 type=${reqType}`));
          }, timeout);
          w.resolve = res; w.reject = rej;
          waiters.push(w);
          sock.write(Buffer.from(encodePDU(reqType, caData, msg)));
        });
      },
      close() { try { sock.destroy(); } catch { /* ignore */ } },
    };
    sock.on('connect', () => resolve(api));
    sock.on('error', reject);
    sock.on('data', (d) => {
      for (const f of framer.push(new Uint8Array(d))) {
        const ab = f.buffer.slice(f.byteOffset, f.byteOffset + f.byteLength);
        const pdu = decodePDU(ab);
        const i = waiters.findIndex((w) => w.type === pdu.uiType);
        if (i >= 0) { const w = waiters.splice(i, 1)[0]; clearTimeout(w.timer); w.resolve(pdu); }
      }
    });
  });
}

function authCaData(username, password) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, username);
  writeExactBytes(caData, 32, 32, textEncoder.encode(account.passwordHashForProtocol(password)));
  return caData;
}

async function withNet(fn) {
  const n = new NetClient();
  setNet(n); // account/friend/chat 命令模块经 getNet() 取用同一实例
  await n.connect(GW_URL);
  try { return await fn(n); } finally { n.close(); }
}

async function main() {
  console.log(`[e2e] 网关=${GW_URL} 直连=${TCP_HOST}:${TCP_PORT} 测试用户=${USER}`);

  // 1) 经网关：验证码 -> 注册新用户
  const reg = await withNet(async (n) => {
    const cap = await account.requestCaptcha(n);
    check('验证码', '经网关获取验证码 status=0 且非空', cap.status === 0 && cap.code.length > 0, `status=${cap.status} code=${cap.code}`);
    return account.register(n, { username: USER, password: PASSWORD, captcha: cap.code });
  });
  check('注册', '经网关注册新用户成功', reg.success === true, `ret=${reg.ret} name=${reg.name || ''}`);

  // 2) 经网关：正确密码登录 -> ret=1（M1 底线）
  const gwLogin = await withNet(async (n) => {
    const cap = await account.requestCaptcha(n);
    return account.login(n, { username: USER, password: PASSWORD, captcha: cap.code, clientId: CLIENT_ID });
  });
  check('网关登录', '经网关正确密码登录 ret=1 且回带登录名', gwLogin.ret === 1 && gwLogin.loginName === USER, `ret=${gwLogin.ret} name=${gwLogin.loginName || ''}`);

  // 3) 经网关：错误密码登录 -> ret=-1（证明不是恒真通过）
  const badLogin = await withNet(async (n) => {
    const cap = await account.requestCaptcha(n);
    return account.login(n, { username: USER, password: 'wrong-pass', captcha: cap.code, clientId: CLIENT_ID });
  });
  check('网关错密码', '经网关错误密码登录 ret=-1', badLogin.ret === -1, `ret=${badLogin.ret}`);

  // 4) 直连 TCP：同一 account.js 代码、同一用户登录 -> ret=1，且与网关结果一致（透明性）
  const tcp = await connectRawTcp();
  let tcpLogin;
  try {
    const cap = await account.requestCaptcha(tcp);
    tcpLogin = await account.login(tcp, { username: USER, password: PASSWORD, captcha: cap.code, clientId: CLIENT_ID });
  } finally { tcp.close(); }
  check('直连登录', '直连 TCP 同代码登录 ret=1', tcpLogin.ret === 1, `ret=${tcpLogin.ret}`);
  check('透明性', '网关与直连 TCP 登录结果一致（ret 与登录名相同）',
    tcpLogin.ret === gwLogin.ret && tcpLogin.loginName === gwLogin.loginName,
    `gw=(${gwLogin.ret},${gwLogin.loginName}) tcp=(${tcpLogin.ret},${tcpLogin.loginName})`);

  const nfail = failures.length;
  console.log(`\nSUMMARY: ${passCount}/${passCount + nfail} PASS, ${nfail} FAIL${nfail ? ' -> ' + failures.join(',') : ''}`);
  // 用 exitCode 让 Node 自然退出并刷新 stdout（Windows 管道下 process.exit 会截断异步写）。
  process.exitCode = nfail === 0 ? 0 : 1;
}

main().catch((e) => { console.error('[e2e] 异常:', e); process.exitCode = 2; });
