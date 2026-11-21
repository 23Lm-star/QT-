// REQ-AUTH-CONNECTION-001 Web 会话专项：验证前端会话命令、心跳 clientId 和失效事件分发。
// 该脚本使用最小内存网关替身，避免测试修改真实账号、Redis 或浏览器存储。
const SRC = new URL('../frontend/src/', import.meta.url);
const { decodePDU, encodePDU } = await import(new URL('protocol/pdu.js', SRC));
const { T } = await import(new URL('protocol/msgtypes.js', SRC));
const account = await import(new URL('protocol/commands/account.js', SRC));
const session = await import(new URL('protocol/session.js', SRC));
const { NetClient } = await import(new URL('protocol/client.js', SRC));
const { createPinia, setActivePinia } = await import(new URL('../frontend/node_modules/pinia/dist/pinia.mjs', import.meta.url));
const { useAuthStore } = await import(new URL('stores/auth.js', new URL('../frontend/src/', import.meta.url)));

let passed = 0;
function check(name, ok, detail = '') {
  if (!ok) throw new Error(`${name}${detail ? ` :: ${detail}` : ''}`);
  passed++;
  console.log(`ok  ${name}`);
}

const clientId = '0123456789abcdef0123456789abcdef';
const token = 'abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789';
const username = 'web_auth_probe';

const frames = [];
const fakeNet = {
  request(reqType, respType, caData, msg) {
    frames.push({ reqType, respType, caData: new Uint8Array(caData || []), msg: new Uint8Array(msg || []) });
    const data = new Uint8Array(64);
    const view = new DataView(data.buffer);
    view.setInt32(0, 0, true);
    if (reqType === T.LOGIN_REQUEST) {
      new TextEncoder().encode(username).forEach((b, i) => { data[32 + i] = b; });
      return Promise.resolve(decodePDU(encodePDU(respType, data, new TextEncoder().encode(`${token}\0`))));
    }
    if (reqType === T.SESSION_RESUME_REQUEST) {
      view.setBigUint64(4, 7n, true);
      new TextEncoder().encode(username).forEach((b, i) => { data[32 + i] = b; });
    }
    return Promise.resolve(decodePDU(encodePDU(respType, data)));
  },
};

const login = await account.login(fakeNet, {
  username, password: 'plain-password', captcha: '1234', clientId,
});
check('Web 登录返回恢复凭据', login.ret === 0 && login.resumeToken === token);
const loginFrame = frames.at(-1);
check('登录 caMsg 携带 clientId', new TextDecoder().decode(loginFrame.msg) === '1234\0' + clientId);
check('登录 clientId 严格小写十六进制', session.isValidClientId(new TextDecoder().decode(loginFrame.msg.slice(5))));

const resume = await account.sessionResume(fakeNet, clientId, token);
check('会话恢复响应解析', resume.ret === 0 && resume.loginVersion === 7n && resume.username === username);
const resumeFrame = frames.at(-1);
check('恢复请求 token 带 NUL', new TextDecoder().decode(resumeFrame.msg) === `${token}\0`);

const logout = await account.sessionLogout(fakeNet, clientId);
check('主动退出响应解析', logout.ret === 0);

globalThis.WebSocket = { OPEN: 1, CONNECTING: 0 };
const net = new NetClient();
let sent = null;
net.ws = { readyState: WebSocket.OPEN, send: (buf) => { sent = decodePDU(buf); } };
net.setHeartbeatClientId(clientId);
net.sendFrame(T.HEARTBEAT_REQUEST);
check('心跳 caData 携带 clientId', new TextDecoder().decode(sent.caData.slice(0, 32)) === clientId);

let invalidated = null;
net.onPush(T.SESSION_INVALIDATED_EVENT, (pdu) => { invalidated = session.parseInvalidatedEvent(pdu); });
const eventData = new Uint8Array(64);
new DataView(eventData.buffer).setInt32(0, -3017, true);
new DataView(eventData.buffer).setBigUint64(4, 8n, true);
net._dispatch(decodePDU(encodePDU(T.SESSION_INVALIDATED_EVENT, eventData, new TextEncoder().encode('replaced\0'))));
check('失效事件推送解析', invalidated && invalidated.reason === -3017 && invalidated.version === 8n);

// Pinia 状态机：断线保留登录态，重连优先恢复，123 清除本地凭据。
const storage = new Map([
  ['cloudpan.clientId', clientId],
  ['cloudpan.resumeToken', token],
  ['cloudpan.resumeUser', username],
]);
globalThis.localStorage = {
  getItem: (key) => storage.get(key) || null,
  setItem: (key, value) => storage.set(key, String(value)),
  removeItem: (key) => storage.delete(key),
};
globalThis.location = { protocol: 'http:', host: '127.0.0.1:8080' };
setActivePinia(createPinia());
const auth = useAuthStore();
auth.init();
const authNet = (await import(new URL('protocol/net.js', SRC))).getNet();
let resumeCalls = 0;
authNet.request = (reqType, respType) => {
  const data = new Uint8Array(64);
  const view = new DataView(data.buffer);
  if (reqType === T.SESSION_RESUME_REQUEST) {
    resumeCalls++;
    view.setInt32(0, 0, true);
    view.setBigUint64(4, 9n, true);
    new TextEncoder().encode(username).forEach((b, i) => { data[32 + i] = b; });
  } else if (reqType === T.SESSION_LOGOUT_REQUEST) {
    view.setInt32(0, 0, true);
  }
  return Promise.resolve(decodePDU(encodePDU(respType, data)));
};
authNet.connect = async () => {
  authNet.connected = true;
  for (const handler of authNet.openHandlers) handler();
};
await auth.connect();
check('断线前自动恢复登录态', auth.loggedIn && auth.sessionActive && resumeCalls === 1);
authNet.connected = false;
for (const handler of authNet.closeHandlers) handler({ code: 1006, reason: '' });
check('断线保留恢复凭据', auth.loggedIn && !auth.sessionActive && storage.get('cloudpan.resumeToken') === token);
await auth.reconnect();
check('手动重连优先恢复', auth.loggedIn && auth.sessionActive && resumeCalls === 2);
await auth.logout();
check('119 退出保留连接并清凭据', authNet.connected && !auth.loggedIn && !storage.has('cloudpan.resumeToken'));

// 重新放入测试凭据，确认 123 事件清理路径。
auth.resumeToken = token;
auth.tokenUser = username;
auth.loggedIn = true;
auth.sessionActive = true;
storage.set('cloudpan.resumeToken', token);
authNet._dispatch(decodePDU(encodePDU(T.SESSION_INVALIDATED_EVENT, eventData)));
check('123 失效事件清理凭据', !auth.loggedIn && !auth.resumeToken && !storage.has('cloudpan.resumeToken'));

console.log(`\n[web_auth_recovery_e2e] 全部通过：${passed} 项断言`);
