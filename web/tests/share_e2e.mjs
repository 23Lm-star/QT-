// M5 端到端：三用户经 WebSocket 网关对真实 Server.exe 验证好友文件分享（本期范围）。
// 覆盖：33→34、35 push、36→37（含离线接收者丢失、重复接受失败、非本人源拒绝转发、
// 多接收人逐个邀请与接受）。
// 外链分享 52-57 暂缓：服务端 resolvePublishLink 只复制物理实体，对象存储文件取回失败
// （见 docs/changes/REQ-WEB-001/记录.md 的 M5 阻塞节）；协议层单测仍在 protocol_smoke。
// 前置：dist/Server.exe 监听 5000，网关监听 8080（run_m5_e2e.py 编排与清理）。
// 运行：node web/tests/share_e2e.mjs
import assert from 'node:assert';
import crypto from 'node:crypto';

const SRC = '../frontend/src/protocol';
const { NetClient } = await import(`${SRC}/client.js`);
const account = await import(`${SRC}/commands/account.js`);
const drive = await import(`${SRC}/commands/drive.js`);
const share = await import(`${SRC}/commands/share.js`);
const friend = await import(`${SRC}/commands/friend.js`);
const { T } = await import(`${SRC}/msgtypes.js`);

const GW_URL = process.env.GW_URL || 'ws://127.0.0.1:8080/ws';
const PASSWORD = '123456';
const ts = Math.floor(Date.now() / 1000);
const USER_A = process.env.E2E_USER_A || `webts_a_${ts}`;
const USER_B = process.env.E2E_USER_B || `webts_b_${ts}`;
const USER_C = process.env.E2E_USER_C || `webts_c_${ts}`;
const ROOT = './filesys';
const CID = 'e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5';

let tidCounter = 0n;
const allocTid = () => (tidCounter += 1n);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const sha256Hex = (buf) => crypto.createHash('sha256').update(buf).digest('hex');
function randBuf(n) { const b = Buffer.alloc(n); crypto.randomFillSync(b); return new Uint8Array(b); }

let passCount = 0;
const failures = [];
function check(id, desc, cond, detail) {
  if (cond) { passCount++; console.log(`[PASS] ${id} ${desc} :: ${detail}`); }
  else { failures.push(id); console.log(`[FAIL] ${id} ${desc} :: ${detail}`); }
}

function waitForPush(net, type, predicate, timeout = 15000, desc = '') {
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
  async register() {
    const n = new NetClient();
    await n.connect(GW_URL);
    try {
      const cap = await account.requestCaptcha(n);
      assert.strictEqual(cap.status, 0, `验证码失败 status=${cap.status}`);
      const r = await account.register(n, { username: this.name, password: PASSWORD, captcha: cap.code });
      assert.ok(r.success, `注册失败 ret=${r.ret}`);
    } finally { n.close(); }
  }
  async login() {
    this.net = new NetClient();
    await this.net.connect(GW_URL);
    const cap = await account.requestCaptcha(this.net);
    assert.strictEqual(cap.status, 0, `验证码失败 status=${cap.status}`);
    const r = await account.login(this.net, { username: this.name, password: PASSWORD, captcha: cap.code, clientId: CID });
    assert.strictEqual(r.ret, 1, `登录失败 ret=${r.ret}`);
  }
  close() { if (this.net) { this.net.close(); this.net = null; } }
}

function twoName(first, second) {
  const caData = new Uint8Array(64);
  const a = new TextEncoder().encode(first);
  const b = new TextEncoder().encode(second);
  caData.set(a.subarray(0, 31), 0);
  caData.set(b.subarray(0, 31), 32);
  return caData;
}

async function makeFriends(a, b) {
  const w9 = waitForPush(b.net, T.ADDFRIEND_REQUEST,
    (p) => friend.parseIncomingFriendRequest(p).requester === a.name, 15000, '(push 9)');
  a.net.sendFrame(T.ADDFRIEND_REQUEST, twoName(a.name, b.name));
  await w9;
  const wA = waitForPush(a.net, T.ADDFRIEND_RESPOND, (p) => friend.parseFriendResult(p).ret === 1, 15000, '(push 10 A)');
  const wB = waitForPush(b.net, T.ADDFRIEND_RESPOND, (p) => friend.parseFriendResult(p).ret === 1, 15000, '(push 10 B)');
  b.net.sendFrame(T.ADDFRIEND_RESPOND, twoName(b.name, a.name));
  await Promise.all([wA, wB]);
}

// 上传编排（镜像 stores/transfer.js）：29/30 → 31/32 停等；测试省略 97 秒传预检。
async function uploadFile(s, { name, dirAbs, data }) {
  const net = s.net;
  const tid = allocTid();
  const initF = drive.buildUploadInitFrame({ name, fileSize: BigInt(data.length), tid, dirAbsPath: dirAbs });
  const w30 = waitForPush(net, T.UPLOADFILE_INIT_RESPOND,
    (p) => drive.parseUploadInitResponse(p).tid === tid, 15000, '(30)');
  net.sendFrame(initF.type, initF.caData, initF.msg);
  const r30 = drive.parseUploadInitResponse(await w30);
  assert.ok(r30.ret, '29 被拒绝（并发上限？）');
  let offset = Number(r30.resumeOffset);
  const chunkSize = drive.chunkSizeFor(data.length);
  while (offset < data.length) {
    const chunk = data.subarray(offset, Math.min(offset + chunkSize, data.length));
    const f = drive.buildUploadChunkFrame({ offset: BigInt(offset), tid, chunk });
    const w32 = waitForPush(net, T.UPLOADFILE_DATA_RESPOND,
      (p) => drive.parseUploadAck(p).tid === tid, 15000, '(32)');
    net.sendFrame(f.type, f.caData, f.msg);
    const ack = drive.parseUploadAck(await w32);
    assert.ok(ack.ret, '31 被拒绝');
    assert.strictEqual(Number(ack.nextOffset), offset + chunk.length, '32 nextOffset 不符');
    offset += chunk.length;
  }
}

async function downloadFile(s, { serverAbs }) {
  const net = s.net;
  const tid = allocTid();
  const parts = [];
  let finResolve = null;
  const finP = new Promise((r) => { finResolve = r; });
  const offData = net.onPush(T.DOWNLOADFILE_DATA, (pdu) => {
    const d = drive.parseDownloadData(pdu);
    if (d.tid === tid) parts.push(Buffer.from(d.chunk));
  });
  const offFin = net.onPush(T.DOWNLOADFILE_FINISH, (pdu) => {
    const f = drive.parseDownloadFinish(pdu);
    if (f.tid === tid) finResolve(f.success);
  });
  const guard = new Promise((_, rej) => setTimeout(() => rej(new Error('等待 43 完成超时')), 90000));
  guard.catch(() => {});
  try {
    const f = drive.buildDownloadFrame({ name: share.fileNameOf(serverAbs), resumeOffset: 0n, tid, serverAbsPath: serverAbs });
    const w41 = waitForPush(net, T.DOWNLOADFILE_RESPOND,
      (p) => drive.parseDownloadResponse(p).tid === tid, 15000, '(41)');
    net.sendFrame(f.type, f.caData, f.msg);
    const r41 = drive.parseDownloadResponse(await w41);
    assert.ok(r41.ret, `40 被拒绝：${serverAbs}`);
    const success = await Promise.race([finP, guard]);
    return { success, data: Buffer.concat(parts), size: Number(r41.fileSize) };
  } finally { offData(); offFin(); }
}

async function listNames(s, absPath) {
  const entries = await drive.listDirectory(absPath, s.net);
  return entries.map((e) => e.name);
}

async function main() {
  console.log(`[share_e2e] 网关=${GW_URL} 用户=${USER_A}/${USER_B}/${USER_C} root=${ROOT}`);
  const A = new Session(USER_A);
  const B = new Session(USER_B);
  const C = new Session(USER_C);
  for (const s of [A, B, C]) await s.register();
  await A.login();
  await B.login();
  try {
    // ===== 好友关系（网页 UI 只允许分享给好友；服务端本身不校验） =====
    await makeFriends(A, B);
    const aFriends = await friend.fetchFriends(A.net);
    check('F1', '好友关系建立', aFriends.includes(USER_B), `A 好友=${aFriends.join(',')}`);

    // ===== 好友文件分享 =====
    const fileData = randBuf(100 * 1024);
    const fileName = `share_${ts}.bin`;
    const srcAbs = `${ROOT}/${USER_A}/${fileName}`;
    await uploadFile(A, { name: fileName, dirAbs: `${ROOT}/${USER_A}`, data: fileData });
    const aRoot = await listNames(A, `${ROOT}/${USER_A}`);
    check('S1', 'A 上传分享源成功', aRoot.includes(fileName), `A 根目录=${aRoot.join(',')}`);

    // 33→34 + B 收 35
    const w35 = waitForPush(B.net, T.SHAREFILE_AGREE_REQUEST,
      (p) => share.parseShareInvite(p).fileName === fileName, 15000, '(push 35)');
    const f33 = share.buildShareFileFrame({ sharerName: USER_A, receivers: [USER_B], sourceAbsPath: srcAbs });
    await A.net.request(f33.type, T.SHAREFILE_RESPOND, f33.caData, f33.msg);
    const inv35 = share.parseShareInvite(await w35);
    check('S2', '35 邀请 push 到在线好友', inv35.sharer === USER_A && inv35.sourceAbsPath === srcAbs,
      `sharer=${inv35.sharer} file=${inv35.fileName}`);

    // 36→37 接收；文件落 B 用户根
    const accepted = await share.acceptShare({ receiverName: USER_B, sourceAbsPath: inv35.sourceAbsPath }, B.net);
    check('S3', '36→37 接收成功', accepted === true, `ret=${accepted}`);
    const bRoot = await listNames(B, `${ROOT}/${USER_B}`);
    check('S4', '接收文件出现在 B 云盘根目录', bRoot.includes(fileName), `B 根目录=${bRoot.join(',')}`);

    const saved = await downloadFile(B, { serverAbs: `${ROOT}/${USER_B}/${fileName}` });
    check('S5', '接收文件内容与源逐字节一致', saved.success && sha256Hex(saved.data) === sha256Hex(Buffer.from(fileData)),
      `size=${saved.size} sha=${sha256Hex(saved.data).slice(0, 12)}`);

    // 重复接受（握手已消费）→ 37 false
    const dupAccept = await share.acceptShare({ receiverName: USER_B, sourceAbsPath: srcAbs }, B.net);
    check('S6', '重复接受同一分享失败（37 ret=false）', dupAccept === false, `ret=${dupAccept}`);

    // 非本人源：A 声称分享 B 的文件 → 34 照回，但 B 不应收到 35
    let leaked = 0;
    const offLeak = B.net.onPush(T.SHAREFILE_AGREE_REQUEST, () => { leaked++; });
    const f33Bad = share.buildShareFileFrame({
      sharerName: USER_A, receivers: [USER_B], sourceAbsPath: `${ROOT}/${USER_B}/${fileName}`,
    });
    await A.net.request(f33Bad.type, T.SHAREFILE_RESPOND, f33Bad.caData, f33Bad.msg);
    await sleep(1200);
    offLeak();
    check('S7', '分享他人文件被服务端静默丢弃（不转发 35）', leaked === 0, `35 次数=${leaked}`);

    // 离线接收者：34 照回但登录后也收不到（与桌面端一致）
    const f33Off = share.buildShareFileFrame({ sharerName: USER_A, receivers: [USER_C], sourceAbsPath: srcAbs });
    await A.net.request(f33Off.type, T.SHAREFILE_RESPOND, f33Off.caData, f33Off.msg);
    await C.login();
    let cInvites = 0;
    const offC = C.net.onPush(T.SHAREFILE_AGREE_REQUEST, () => { cInvites++; });
    await sleep(1500);
    offC();
    check('S8', '离线接收者的邀请在线后不会补发', cInvites === 0, `35 次数=${cInvites}`);

    // 多接收人：A 一次分享给 B、C（C 已在线），两人各收一条 35；C 接受后落 C 根目录
    const w35B = waitForPush(B.net, T.SHAREFILE_AGREE_REQUEST,
      (p) => share.parseShareInvite(p).fileName === fileName, 15000, '(push 35 B)');
    const w35C = waitForPush(C.net, T.SHAREFILE_AGREE_REQUEST,
      (p) => share.parseShareInvite(p).fileName === fileName, 15000, '(push 35 C)');
    const f33Multi = share.buildShareFileFrame({ sharerName: USER_A, receivers: [USER_B, USER_C], sourceAbsPath: srcAbs });
    await A.net.request(f33Multi.type, T.SHAREFILE_RESPOND, f33Multi.caData, f33Multi.msg);
    const [invB, invC] = await Promise.all([w35B, w35C]).then(([b, c]) => [share.parseShareInvite(b), share.parseShareInvite(c)]);
    check('S9', '一次分享多位接收人，各自收到 35',
      invB.sharer === USER_A && invC.sharer === USER_A, `B=${invB.fileName} C=${invC.fileName}`);
    const okC = await share.acceptShare({ receiverName: USER_C, sourceAbsPath: invC.sourceAbsPath }, C.net);
    const cRoot = await listNames(C, `${ROOT}/${USER_C}`);
    check('S10', '第二位接收人接受成功并落自己根目录',
      okC === true && cRoot.includes(fileName), `ret=${okC} C 根目录=${cRoot.join(',')}`);
  } finally {
    for (const s of [A, B, C]) s.close();
  }

  console.log(`\n[share_e2e] 通过 ${passCount} / ${passCount + failures.length}`);
  if (failures.length) {
    console.log('[share_e2e] 失败项:', failures.join(','));
    process.exitCode = 1;
  }
}

await main();
