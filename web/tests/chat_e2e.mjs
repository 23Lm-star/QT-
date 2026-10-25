// M2 端到端：双用户经 WebSocket 网关对真实 Server.exe 验证好友与私聊全流程。
// 覆盖：注册/登录（独立连接避开验证码冷却）、在线列表、加好友（push 9/双方 push 10）、
// 好友列表、实时私聊（push 15，UTF-8 中文双向）、在线消息不重复补发、
// 离线补发恰一次（17/18 issuccess 语义）、删好友双向清除。
// 前置：dist/Server.exe 监听 5000，网关监听 8080（run_m2_e2e.py 负责编排与清理）。
// 运行：node web/tests/chat_e2e.mjs
import assert from 'node:assert';

const SRC = '../frontend/src/protocol';
const { NetClient } = await import(`${SRC}/client.js`);
const account = await import(`${SRC}/commands/account.js`);
const friendCmd = await import(`${SRC}/commands/friend.js`);
const chatCmd = await import(`${SRC}/commands/chat.js`);
const { T } = await import(`${SRC}/msgtypes.js`);

const GW_URL = process.env.GW_URL || 'ws://127.0.0.1:8080/ws';
const PASSWORD = '123456';
const ts = Math.floor(Date.now() / 1000);
const UA = process.env.E2E_USER_A || `webta_${ts}`;
const UB = process.env.E2E_USER_B || `webtb_${ts}`;
const CID = 'c0ffeec0ffeec0ffeec0ffeec0ffee00';

let passCount = 0;
const failures = [];
function check(id, desc, cond, detail) {
  if (cond) { passCount++; console.log(`[PASS] ${id} ${desc} :: ${detail}`); }
  else { failures.push(id); console.log(`[FAIL] ${id} ${desc} :: ${detail}`); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// 等待满足谓词的 push 帧；返回该帧。
function waitForPush(net, type, predicate, timeout = 8000, desc = '') {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { off(); reject(new Error(`等待 push ${type} 超时 ${desc}`)); }, timeout);
    const off = net.onPush(type, (pdu) => {
      let hit = false;
      try { hit = predicate(pdu); } catch { hit = false; }
      if (hit) { clearTimeout(timer); off(); resolve(pdu); }
    });
  });
}

class Session {
  constructor(name) { this.name = name; this.net = null; }
  // 注册用独立短连接（验证码按连接冷却，注册消费后同连接登录会撞冷却）。
  async register() {
    const n = new NetClient();
    await n.connect(GW_URL);
    try {
      const cap = await account.requestCaptcha(n);
      assert.strictEqual(cap.status, 0, `验证码获取失败 status=${cap.status}`);
      const r = await account.register(n, { username: this.name, password: PASSWORD, captcha: cap.code });
      assert.ok(r.success, `注册失败 ret=${r.ret}`);
    } finally { n.close(); }
  }
  async login() {
    this.net = new NetClient();
    await this.net.connect(GW_URL);
    const cap = await account.requestCaptcha(this.net);
    assert.strictEqual(cap.status, 0, `验证码获取失败 status=${cap.status}`);
    const r = await account.login(this.net, { username: this.name, password: PASSWORD, captcha: cap.code, clientId: CID });
    assert.strictEqual(r.ret, 1, `登录失败 ret=${r.ret}`);
  }
  close() { if (this.net) { this.net.close(); this.net = null; } }
}

async function main() {
  console.log(`[chat_e2e] 网关=${GW_URL} 用户A=${UA} 用户B=${UB}`);
  const A = new Session(UA);
  const B = new Session(UB);

  await A.register();
  await B.register();
  check('注册', '双用户经网关注册成功', true, `${UA} / ${UB}`);
  await A.login();
  await B.login();
  check('登录', '双用户登录成功并保持长连接', true, '');

  try {
    // 1) 在线列表：A 应看到 B，且不含自己（服务端已排除请求者）。
    const onlineA = await friendCmd.fetchOnlineUsers(A.name, A.net);
    check('在线列表', 'A 的在线列表包含 B 且不含自己',
      onlineA.includes(UB) && !onlineA.includes(UA), `online=${JSON.stringify(onlineA)}`);

    // 2) A 发好友申请（9，发送即忘）→ B 收到 push 9。
    const reqOnB = waitForPush(B.net, T.ADDFRIEND_REQUEST,
      (pdu) => friendCmd.parseIncomingFriendRequest(pdu).requester === UA, 8000, '(B 收申请)');
    friendCmd.sendAddFriendRequest(A.name, B.name, A.net);
    await reqOnB;
    check('好友申请', 'B 收到 push 9 且申请人为 A', true, '');

    // 3) B 同意（10）→ 双方都收到 push 10 ret=1。
    const resOnA = waitForPush(A.net, T.ADDFRIEND_RESPOND,
      (pdu) => friendCmd.parseFriendResult(pdu).ret === 1, 8000, '(A 收结果)');
    const resOnB = waitForPush(B.net, T.ADDFRIEND_RESPOND,
      (pdu) => friendCmd.parseFriendResult(pdu).ret === 1, 8000, '(B 收结果)');
    friendCmd.sendAddFriendAgree(B.name, A.name, B.net);
    await Promise.all([resOnA, resOnB]);
    check('同意结果', '同意后双方均收到 push 10 ret=1', true, '');

    // 4) 好友列表互含。
    const [fa, fb] = await Promise.all([friendCmd.fetchFriends(A.net), friendCmd.fetchFriends(B.net)]);
    check('好友列表', '双方好友列表互含', fa.includes(UB) && fb.includes(UA), `A=[${fa}] B=[${fb}]`);

    // 5) 实时私聊 A→B（UTF-8 中英混合）。
    const msgAB = 'hello from A 你好 123';
    const pushOnB = waitForPush(B.net, T.CHAT_REQUEST, (pdu) => {
      const c = chatCmd.parseIncomingChat(pdu);
      return c.from === UA && c.text === msgAB;
    }, 8000, '(B 收消息)');
    chatCmd.sendChat(A.name, B.name, msgAB, A.net);
    const c1 = chatCmd.parseIncomingChat(await pushOnB);
    check('私聊A→B', 'B 实时收到 push 15，发送者/正文/时间齐全',
      c1.from === UA && c1.text === msgAB && c1.time.length > 0, `time="${c1.time}"`);

    // 6) 实时私聊 B→A（中文）。
    const msgBA = '中文回复 from B';
    const pushOnA = waitForPush(A.net, T.CHAT_REQUEST, (pdu) => {
      const c = chatCmd.parseIncomingChat(pdu);
      return c.from === UB && c.text === msgBA;
    }, 8000, '(A 收回复)');
    chatCmd.sendChat(B.name, A.name, msgBA, B.net);
    await pushOnA;
    check('私聊B→A', 'A 实时收到 B 的中文回复', true, '');

    // 7) 在线送达的消息（issuccess=1）不应出现在 B 的补发收件箱。
    const inboxB = await chatCmd.fetchChatHistory(B.name, A.name, B.net);
    check('在线不补发', '17/18 不重复下发在线已推送消息',
      !inboxB.some((r) => r.text === msgAB), `inbox=${JSON.stringify(inboxB)}`);

    // 8) 离线补发：B 断开 → A 发消息（issuccess=0，无 push）→ B 重连拉取应命中且仅一次。
    B.close();
    await sleep(1500); // 等服务端将 B 标记离线（避免幽灵在线导致 issuccess=1 丢消息）
    const msgOff = 'offline msg for B 离线补发';
    chatCmd.sendChat(A.name, B.name, msgOff, A.net);
    await sleep(300);
    await B.login();
    const inbox1 = await chatCmd.fetchChatHistory(B.name, A.name, B.net);
    const inbox2 = await chatCmd.fetchChatHistory(B.name, A.name, B.net);
    check('离线补发', 'B 重连后 17/18 拉到离线期间的消息',
      inbox1.some((r) => r.text === msgOff), `inbox1=${JSON.stringify(inbox1)}`);
    check('补发仅一次', '第二次拉取不再含该消息（取回即标记已投递）',
      !inbox2.some((r) => r.text === msgOff), `inbox2=${JSON.stringify(inbox2)}`);

    // 9) 删好友：A 发起，双向 friend 行删除，双方列表不再互含。
    const del = await friendCmd.deleteFriend(B.name, A.name, A.net);
    const [fa2, fb2] = await Promise.all([friendCmd.fetchFriends(A.net), friendCmd.fetchFriends(B.net)]);
    check('删好友', '删除成功且双方列表均不再含对方',
      del === true && !fa2.includes(UB) && !fb2.includes(UA), `del=${del} A=[${fa2}] B=[${fb2}]`);
  } finally {
    A.close();
    B.close();
  }

  const nfail = failures.length;
  console.log(`\nSUMMARY: ${passCount}/${passCount + nfail} PASS, ${nfail} FAIL${nfail ? ' -> ' + failures.join(',') : ''}`);
  process.exitCode = nfail === 0 ? 0 : 1;
}

main().catch((e) => { console.error('[chat_e2e] 异常:', e); process.exitCode = 2; });
