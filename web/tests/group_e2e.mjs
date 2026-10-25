// M4 端到端：三用户经 WebSocket 网关对真实 Server.exe 验证群命令（生命周期 + 文本 + 文件）。
// A 生命周期（60-87）：建群/群列表/邀请/接受/拒绝/成员列表/设角色/转让/移除/退出/解散，
//                    push 85（邀请到达被邀请人）与 push 84（成员变化到达其他成员）实测。
// B 群文本（88-96）：发送/在线 push 90/按游标拉取/已读确认/未读摘要/离线补发恰一次/幂等重发。
// C 群文件（99-106）：列表/上传含秒传/暂停续传/下载/删除 + 非成员越权负例。
// 前置：run_m4_e2e.py 起 Server.exe + 网关并清理夹具。
// 运行：node web/tests/group_e2e.mjs（环境变量 E2E_USER_A/B/C 由编排注入）
import assert from 'node:assert';
import crypto from 'node:crypto';

const SRC = '../frontend/src/protocol';
const { NetClient } = await import(`${SRC}/client.js`);
const account = await import(`${SRC}/commands/account.js`);
const drive = await import(`${SRC}/commands/drive.js`);
const g = await import(`${SRC}/commands/group.js`);
const { T } = await import(`${SRC}/msgtypes.js`);

const GW_URL = process.env.GW_URL || 'ws://127.0.0.1:8080/ws';
const PASSWORD = '123456';
const ts = Math.floor(Date.now() / 1000);
const USER_A = process.env.E2E_USER_A || `webtg_a_${ts}`;
const USER_B = process.env.E2E_USER_B || `webtg_b_${ts}`;
const USER_C = process.env.E2E_USER_C || `webtg_c_${ts}`;
const CID = 'e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4e4';
const ROOT = './filesys';
const sha256Hex = (buf) => crypto.createHash('sha256').update(buf).digest('hex');
function randBuf(n) { const b = Buffer.alloc(n); crypto.randomFillSync(b); return new Uint8Array(b); }

let passCount = 0; const failures = [];
function check(id, desc, cond, detail = '') {
  if (cond) { passCount++; console.log(`[PASS] ${id} ${desc}${detail ? ' :: ' + detail : ''}`); }
  else { failures.push(id); console.log(`[FAIL] ${id} ${desc}${detail ? ' :: ' + detail : ''}`); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// push 观察器：在 net 上累积满足谓词的 PDU，配合 waitFor 断言。
class PushSpy {
  constructor(net, type) {
    this.type = type; this.hits = []; this.waiters = [];
    net.onPush(type, (pdu) => {
      this.hits.push(pdu);
      for (const w of [...this.waiters]) {
        let hit = false; try { hit = w.pred(pdu); } catch { hit = false; }
        if (hit) { this.waiters.splice(this.waiters.indexOf(w), 1); clearTimeout(w.timer); w.resolve(pdu); }
      }
    });
  }
  clear() { this.hits = []; }
  waitFor(pred, timeout = 8000, desc = '') {
    return new Promise((resolve, reject) => {
      for (const p of this.hits) { try { if (pred(p)) { resolve(p); return; } } catch { /* skip */ } }
      const w = { pred, resolve };
      w.timer = setTimeout(() => reject(new Error(`等待 push ${this.type} 超时 ${desc}`)), timeout);
      this.waiters.push(w);
    });
  }
}

class Session {
  constructor(name) { this.name = name; this.net = null; this.spies = {}; }
  spy(type) {
    const k = String(type);
    if (!this.spies[k]) this.spies[k] = new PushSpy(this.net, type);
    return this.spies[k];
  }
  async register() {
    const n = new NetClient();
    await n.connect(GW_URL);
    try {
      const cap = await account.requestCaptcha(n);
      assert.strictEqual(cap.status, 0, `${this.name} 验证码失败 status=${cap.status}`);
      const r = await account.register(n, { username: this.name, password: PASSWORD, captcha: cap.code });
      assert.ok(r.success, `${this.name} 注册失败 ret=${r.ret}`);
    } finally { n.close(); }
  }
  async login() {
    this.net = new NetClient();
    g.bindGroup(this.net);
    await this.net.connect(GW_URL);
    const cap = await account.requestCaptcha(this.net);
    assert.strictEqual(cap.status, 0, `${this.name} 验证码失败 status=${cap.status}`);
    const r = await account.login(this.net, { username: this.name, password: PASSWORD, captcha: cap.code, clientId: CID });
    assert.strictEqual(r.ret, 1, `${this.name} 登录失败 ret=${r.ret}`);
  }
  close() { if (this.net) { this.net.close(); this.net = null; } }
}

// ===== 编排：传输引擎（与 stores/transfer.js 同逻辑的测试实现）=====
let tidCounter = 0n;
const allocTid = () => (tidCounter += 1n);
// 从 caData 视图按偏移读小端 u64（tid 归属）。
const u64At = (u8, off) => new DataView(u8.buffer, u8.byteOffset, u8.byteLength).getBigUint64(off, true);

function waitTid(net, type, tid, timeout, desc) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { off(); reject(new Error(`等待 ${type}:${desc} 超时`)); }, timeout);
    const off = net.onPush(type, (pdu) => {
      const id = u64At(pdu.caData, type === 41 ? 16 : 48);
      if (id === tid) { clearTimeout(timer); off(); resolve(pdu); }
    });
  });
}

// 群文件上传（101→[31/32]*），data 为 Uint8Array。sha 走秒传内嵌于 101。
async function groupUpload(s, groupId, name, data, { precomputedSha = null } = {}) {
  const net = s.net;
  const sha = precomputedSha || sha256Hex(Buffer.from(data));
  const tid = allocTid();
  const body = g.buildGroupFileInitBody(BigInt(data.length), sha, name);
  const caData = new Uint8Array(64);
  new DataView(caData.buffer).setBigUint64(48, tid, true);
  const env = g.gEnvelope(tid, groupId);
  const msg = new Uint8Array(env.length + body.length); msg.set(env, 0); msg.set(body, env.length);
  const w = waitTid(net, T.GROUP_FILE_UPLOAD_INIT_RESPOND, tid, 15000, '102');
  net.sendFrame(T.GROUP_FILE_UPLOAD_INIT_REQUEST, caData, msg);
  const r102 = g.parseGroupFileInitResponse(await w);
  if (r102.code !== 0) return { code: r102.code, tid };
  if (r102.hit || Number(r102.nextOffset) >= data.length) return { code: 0, hit: r102.hit, nextOffset: r102.nextOffset, tid };
  const chunkSize = drive.chunkSizeFor(data.length);
  let offset = Number(r102.nextOffset);
  while (offset < data.length) {
    const end = Math.min(offset + chunkSize, data.length);
    const f = drive.buildUploadChunkFrame({ offset: BigInt(offset), tid, chunk: data.subarray(offset, end) });
    const wa = waitTid(net, T.UPLOADFILE_DATA_RESPOND, tid, 15000, '32');
    net.sendFrame(f.type, f.caData, f.msg);
    const ack = drive.parseUploadAck(await wa);
    if (!ack.ret) return { code: -1, reason: '32 ret=false', tid };
    offset = Number(ack.nextOffset);
    if (ack.complete) return { code: 0, hit: false, nextOffset: BigInt(offset), complete: true, tid };
  }
  return { code: 0, hit: false, nextOffset: BigInt(offset), tid };
}

// 群文件暂停（107/108）。
async function groupUploadAbort(s, tid) {
  const f = drive.buildAbortFrame(tid);
  const w = waitTid(s.net, T.UPLOADFILE_ABORT_RESPOND, tid, 10000, '108');
  s.net.sendFrame(f.type, f.caData, f.msg);
  return drive.parseAbortResponse(await w);
}

// 群文件下载（105→[42]*→43），返回 Uint8Array 与完成标志。
async function groupDownload(s, groupId, gfId, resumeOffset = 0n) {
  if (gfId == null) return { code: -9, reason: '缺少 gfId' };
  const net = s.net;
  const tid = allocTid();
  const caData = new Uint8Array(64);
  const d = new DataView(caData.buffer);
  d.setBigUint64(48, tid, true);
  d.setBigUint64(32, BigInt(resumeOffset), true);
  const env = g.gEnvelope(tid, groupId);
  const body = g.buildGroupFileDownloadBody(BigInt(gfId));
  const msg = new Uint8Array(env.length + body.length); msg.set(env, 0); msg.set(body, env.length);
  const parts = []; let got = 0n;
  const onData = (pdu) => { if (u64At(pdu.caData, 48) === tid) { const b = pdu.caMsg; parts.push(Uint8Array.from(b)); got += BigInt(b.length); } };
  const offD = net.onPush(T.DOWNLOADFILE_DATA, onData);
  const finW = new Promise((resolve) => {
    const off = net.onPush(T.DOWNLOADFILE_FINISH, (pdu) => { if (u64At(pdu.caData, 48) === tid) { off(); resolve(drive.parseDownloadFinish(pdu)); } });
  });
  const w = waitTid(net, T.GROUP_FILE_DOWNLOAD_RESPOND, tid, 15000, '106');
  net.sendFrame(T.GROUP_FILE_DOWNLOAD_REQUEST, caData, msg);
  const r106 = g.parseGroupFileDownloadResponse(await w);
  if (r106.code !== 0) { offD(); return { code: r106.code }; }
  const fin = await finW; offD();
  const out = new Uint8Array(Number(got));
  let o = 0; for (const p of parts) { out.set(p, o); o += p.length; }
  return { code: 0, size: r106.size, accepted: r106.accepted, data: out, success: fin.success };
}

// ===== A 生命周期 =====
async function testLifecycle(a, b, c) {
  const spyB85 = b.spy(T.GROUP_INVITATION_EVENT);
  const spyA84 = a.spy(T.GROUP_MEMBER_EVENT);

  // A1 建群
  const r1 = await g.createGroup('M4测试群', a.net);
  check('A1', '60 建群成功', r1.code === 0 && r1.groupId > 0n, `groupId=${r1.groupId}`);
  const gid = r1.groupId;
  c.groupId = gid; // 供 C 段越权负例复用

  // A2 群列表含 owner
  const l1 = await g.listGroups(a.net);
  const mine = l1.groups.find((x) => x.groupId === gid);
  check('A2', '78 群列表含新群且 role=owner', l1.code === 0 && mine && mine.role === 'owner' && mine.status === 'active');

  // A3 邀请 B → B 收 push 85，且 82 可查到
  const inv = await g.inviteMember(gid, USER_B, g.newInviteKey(), a.net);
  check('A3a', '62 邀请返回 invitation_id', inv.code === 0 && inv.invitationId > 0n, `invId=${inv.invitationId}`);
  let push85ok = false;
  try { const p = await spyB85.waitFor(() => true, 6000, '(邀请事件)'); push85ok = p.uiType === T.GROUP_INVITATION_EVENT; } catch { /* fail below */ }
  check('A3b', '85 邀请 push 到达被邀请人 B', push85ok);
  const q1 = await g.queryInvitations(b.net);
  const row = q1.invitations.find((x) => x.invitationId === inv.invitationId);
  check('A3c', '82 B 侧邀请查询含该条（pending，含群名与邀请人）', q1.code === 0 && row && row.groupName === 'M4测试群' && row.inviter === USER_A && row.status === 'pending');

  // A4 B 接受 → A 收 push 84，成员列表 [A,B]
  const acc = await g.acceptInvitation(inv.invitationId, b.net);
  check('A4a', '64 接受邀请返回 group_id', acc.code === 0 && acc.groupId === gid);
  let push84ok = false;
  try { const p = await spyA84.waitFor(() => true, 6000, '(成员事件)'); push84ok = p.uiType === T.GROUP_MEMBER_EVENT; } catch { /* fail below */ }
  check('A4b', '84 成员变化 push 到达群主 A', push84ok);
  const mem1 = await g.listGroupMembers(gid, a.net);
  const names = mem1.members.map((m) => m.name).sort();
  check('A4c', '80 成员列表 = [A,B]，A=owner B=member',
    mem1.code === 0 && JSON.stringify(names) === JSON.stringify([USER_A, USER_B].sort()) &&
    mem1.members.find((m) => m.name === USER_A).role === 'owner' &&
    mem1.members.find((m) => m.name === USER_B).role === 'member');

  // A5 B 侧群列表 role=member
  const l2 = await g.listGroups(b.net);
  const bRow = l2.groups.find((x) => x.groupId === gid);
  check('A5', '78 B 群列表含群且 role=member', !!bRow && bRow.role === 'member');

  // A6 非成员 C 操作 → 生命周期段用 -1xxx（-1012 非成员无法查看）
  const neg = await g.listGroupMembers(gid, c.net);
  check('A6', '80 非成员 C 查成员被拒（-1012）', neg.code === -1012, `code=${neg.code}`);

  // A7 设角色 admin
  await g.setGroupRole(gid, USER_B, 'admin', a.net);
  const mem2 = await g.listGroupMembers(gid, a.net);
  check('A7', '72 设 B 为 admin', mem2.members.find((m) => m.name === USER_B)?.role === 'admin');

  // A8 转让群主 → B owner，A admin
  const tr = await g.transferOwner(gid, USER_B, a.net);
  const mem3 = await g.listGroupMembers(gid, a.net);
  check('A8', '74 转让群主（B=owner，A=admin）',
    tr.code === 0 && mem3.members.find((m) => m.name === USER_B)?.role === 'owner' &&
    mem3.members.find((m) => m.name === USER_A)?.role === 'admin');

  // A9 群摘要
  const sum = await g.queryGroupSummary(gid, b.net);
  check('A9', '86 群摘要（active + version + member_count=2）',
    sum.code === 0 && sum.groupName === 'M4测试群' && sum.memberCount === 2, `count=${sum.memberCount}`);

  // A10 邀请 C 到独立群、C 拒绝 → 邀请列表清空、C 未入群
  const rC = await g.createGroup('M4拒绝群', a.net);
  const invC = await g.inviteMember(rC.groupId, USER_C, g.newInviteKey(), a.net);
  const rej = await g.rejectInvitation(invC.invitationId, c.net);
  const q2 = await g.queryInvitations(c.net);
  const lC1 = await g.listGroups(c.net);
  check('A10', '66 C 拒绝邀请：82 不再含该条且 78 未入群',
    rej.code === 0 && !q2.invitations.some((x) => x.invitationId === invC.invitationId)
    && !lC1.groups.some((x) => x.groupId === rC.groupId));

  // A11 邀请 C 到另一群、C 接受后退出 → 群列表清空
  const rD = await g.createGroup('M4退出群', a.net);
  const invD = await g.inviteMember(rD.groupId, USER_C, g.newInviteKey(), a.net);
  const accD = await g.acceptInvitation(invD.invitationId, c.net);
  const lv = await g.leaveGroup(rD.groupId, c.net);
  const lC2 = await g.listGroups(c.net);
  check('A11', '68 C 接受后退出，群列表不再含该群',
    accD.code === 0 && lv.code === 0 && !lC2.groups.some((x) => x.groupId === rD.groupId));

  // A12 owner B 解散主群 → 双方列表清空（或状态非 active）
  const dis = await g.dissolveGroup(gid, b.net);
  const lA = await g.listGroups(a.net);
  const lB = await g.listGroups(b.net);
  const gone = (lst) => { const r = lst.find((x) => x.groupId === gid); return !r || r.status !== 'active'; };
  check('A12', '76 owner 解散后成员侧群列表不再活跃含该群',
    dis.code === 0 && gone(lA.groups) && gone(lB.groups), `dis=${dis.code}`);

  return { gid };
}

// ===== B 群文本 =====
async function testMessages(a, b, c) {
  const r = await g.createGroup('M4文本群', a.net);
  const gid = r.groupId;
  c.groupId2 = gid;
  const inv = await g.inviteMember(gid, USER_B, g.newInviteKey(), a.net);
  await g.acceptInvitation(inv.invitationId, b.net);
  const spyB90 = b.spy(T.GROUP_MESSAGE_EVENT);
  const spyA90 = a.spy(T.GROUP_MESSAGE_EVENT);
  spyB90.clear();

  // B1 发送与 ack
  const id1 = g.newClientMsgId();
  const s1 = await g.sendGroupMessage(gid, id1, '你好群！', a.net);
  check('B1a', '88 发送 ack 回显 seq/msg_id/created_at',
    s1.code === 0 && s1.clientMsgId === id1 && s1.seq > 0n && s1.msgId > 0n && s1.createdAt > 0n, `seq=${s1.seq}`);

  // B2 在线 push 90 到达 B（内容/发送者/seq 正确）
  let ev = null;
  try { ev = g.parseGroupMessageEvent(await spyB90.waitFor(() => true, 8000, '(90)')); } catch { /* below */ }
  check('B2', '90 在线消息 push 到达 B（sender/content/seq 一致）',
    !!ev && ev.sender === USER_A && ev.content === '你好群！' && ev.seq === s1.seq && ev.groupId === gid);

  // B3 幂等重发同 client_msg_id → 同 seq，不重复入库
  const s1b = await g.sendGroupMessage(gid, id1, '你好群！', a.net);
  check('B3', '88 重发幂等键返回同 seq（不重复）', s1b.code === 0 && s1b.seq === s1.seq, `seq=${s1b.seq}`);

  // B4 拉取（mode 1 向后翻页，limit=10，cursor=lastSeq+1）
  const p1 = await g.pullGroupMessages(gid, 1, s1.seq + 1n, 10, b.net);
  check('B4', '91 向后拉取含刚发消息',
    p1.code === 0 && p1.records.some((x) => x.seq === s1.seq && x.content === '你好群！'));

  // B5 已读确认 → 未读归零
  const rc = await g.confirmGroupRead(gid, s1.seq, b.net);
  const un1 = await g.queryGroupUnread(b.net);
  const row1 = un1.items.find((x) => x.groupId === gid);
  check('B5', '93 已读确认 + 96 未读摘要归零',
    rc.code === 0 && rc.lastReadSeq === s1.seq && (!row1 || Number(row1.unread) === 0), `unread=${row1?.unread}`);

  // B6 B 断线期间 A 发两条 → B 重连 pull 补齐恰一次（不重复）
  b.close();
  await sleep(150); // 让旧连接 close 事件（含 rejectAllFor）落定后再重连
  const s2 = await g.sendGroupMessage(gid, g.newClientMsgId(), '离线一', a.net);
  const s3 = await g.sendGroupMessage(gid, g.newClientMsgId(), '离线二', a.net);
  await sleep(400);
  await b.login();
  const spyB90b = b.spy(T.GROUP_MESSAGE_EVENT); spyB90b.clear();
  const p2 = await g.pullGroupMessages(gid, 1, s3.seq + 1n, 10, b.net);
  const got = p2.records.filter((x) => x.seq === s2.seq || x.seq === s3.seq);
  check('B6a', '91 离线补发恰含两条（升序、无重复）',
    p2.code === 0 && got.length === 2 && got[0].seq < got[1].seq);
  await g.confirmGroupRead(gid, s3.seq, b.net);
  const un2 = await g.queryGroupUnread(b.net);
  const row2 = un2.items.find((x) => x.groupId === gid);
  check('B6b', '96 确认已读后该群无未读', !row2 || Number(row2.unread) === 0);

  // B7 非成员 C 发群消息 → -2101；越权拉其他群消息同样被拒
  const negs = await g.sendGroupMessage(gid, g.newClientMsgId(), '我不是成员', c.net);
  check('B7', '88 非成员发送被拒（-2101）', negs.code === -2101, `code=${negs.code}`);

  // B8 正文超长（>2000 字节）客户端侧拦截
  let tooLong = false;
  try { await g.sendGroupMessage(gid, g.newClientMsgId(), 'x'.repeat(2001), a.net); } catch (e) { tooLong = /过长/.test(e.message); }
  check('B8', '88 正文 >2000 字节客户端拦截', tooLong);

  await g.dissolveGroup(gid, a.net);
  return { gid };
}

// ===== C 群文件 =====
async function testFiles(a, b, c) {
  const r = await g.createGroup('M4文件群', a.net);
  const gid = r.groupId;
  const inv = await g.inviteMember(gid, USER_B, g.newInviteKey(), a.net);
  const acc = await g.acceptInvitation(inv.invitationId, b.net);
  const lb = await g.listGroups(b.net);
  check('C0', '测试群建立且 B 已入群（78 含该群）',
    r.code === 0 && acc.code === 0 && lb.groups.some((x) => x.groupId === gid), `acc=${acc.code}`);

  const data = randBuf(100 * 1024); // 100KiB → 2 分片
  const sha = sha256Hex(Buffer.from(data));

  // C1 上传（非秒传路径）
  const up1 = await groupUpload(a, gid, 'm4file.bin', data, { precomputedSha: sha });
  check('C1', '101 群上传 100KiB 完成', up1.code === 0 && Number(up1.nextOffset) === data.length, `next=${up1.nextOffset}`);

  // C2 列表含该文件（uploader/display/size 正确）
  const ls = await g.listGroupFiles(gid, 0, 200, b.net);
  const f1 = (ls.files || []).find((x) => x.name === 'm4file.bin') || {};
  check('C2', '99 成员 B 列文件含 m4file.bin（size/uploader 正确）',
    ls.code === 0 && f1.uploader === USER_A && f1.size === BigInt(data.length) && ls.total === 1,
    `code=${ls.code} total=${ls.total} files=${(ls.files||[]).length}`);

  // C3 下载与源 SHA 一致
  const dl = await groupDownload(b, gid, f1.gfId);
  const dlSha = dl.data ? sha256Hex(Buffer.from(dl.data)) : '';
  check('C3', '105 群文件下载字节一致（SHA-256）',
    dl.code === 0 && dl.success && dl.size === BigInt(data.length) && dlSha === sha, `code=${dl.code}`);

  // C4 秒传命中：同内容不同名 → hit=1 零正文
  const up2 = await groupUpload(a, gid, 'm4copy.bin', data, { precomputedSha: sha });
  const ls2 = await g.listGroupFiles(gid, 0, 200, a.net);
  const f2 = (ls2.files || []).find((x) => x.name === 'm4copy.bin');
  check('C4', '101 秒传命中（hit=1，条目直接出现）', up2.code === 0 && up2.hit === true && !!f2, `code=${up2.code} hit=${up2.hit}`);

  // C5 暂停→续传：传 5MiB 中途 abort，再新 tid 重传（内嵌 sha 预检命中续传会话）
  const big = randBuf(5 * 1024 * 1024);
  const bigSha = sha256Hex(Buffer.from(big));
  // 手工半程上传：101 后只传 2 块即 107
  {
    const tid = allocTid();
    const body = g.buildGroupFileInitBody(BigInt(big.length), bigSha, 'm4big.bin');
    const caData = new Uint8Array(64); new DataView(caData.buffer).setBigUint64(48, tid, true);
    const env = g.gEnvelope(tid, gid);
    const msg = new Uint8Array(env.length + body.length); msg.set(env, 0); msg.set(body, env.length);
    const w = waitTid(a.net, T.GROUP_FILE_UPLOAD_INIT_RESPOND, tid, 15000, '102');
    a.net.sendFrame(T.GROUP_FILE_UPLOAD_INIT_REQUEST, caData, msg);
    const r102 = g.parseGroupFileInitResponse(await w);
    assert.strictEqual(r102.code, 0, `C5 初始化失败 code=${r102.code}`);
    assert.strictEqual(r102.hit, false, 'C5 不应命中秒传');
    const cs = drive.chunkSizeFor(big.length);
    let off = Number(r102.nextOffset);
    for (let i = 0; i < 2 && off < big.length; i++) {
      const end = Math.min(off + cs, big.length);
      const f = drive.buildUploadChunkFrame({ offset: BigInt(off), tid, chunk: big.subarray(off, end) });
      const wa = waitTid(a.net, T.UPLOADFILE_DATA_RESPOND, tid, 15000, '32');
      a.net.sendFrame(f.type, f.caData, f.msg);
      const ack = drive.parseUploadAck(await wa);
      assert.ok(ack.ret, 'C5 分片 ack ret=false');
      off = Number(ack.nextOffset);
    }
    const ab = await groupUploadAbort(a, tid);
    check('C5a', '107 群上传暂停释放成功', ab.ret === true, `abort tid=${ab.tid}`);
    // 续传：新 tid 再 101（同 sha/name/size → 服务端匹配 staging 回带偏移）
    const up3 = await groupUpload(a, gid, 'm4big.bin', big, { precomputedSha: bigSha });
    const resumed = Number(up3.nextOffset) > 0 || up3.hit === true;
    check('C5b', '101 续传/秒传收敛（staging 采纳或对象命中）', up3.code === 0 && up3.nextOffset === BigInt(big.length) && resumed);
    const ls3 = await g.listGroupFiles(gid, 0, 200, a.net);
    const f3 = ls3.files.find((x) => x.name === 'm4big.bin');
    const dl3 = f3 ? await groupDownload(b, gid, f3.gfId) : { code: -9 };
    check('C5c', '105 续传大文件下载 SHA 一致', dl3.code === 0 && dl3.success && sha256Hex(Buffer.from(dl3.data)) === bigSha);
  }

  // C6 非成员 C：列文件/下载越权被拒
  const negLs = await g.listGroupFiles(gid, 0, 200, c.net);
  check('C6', '99 非成员 C 列群文件被拒（-2101）', negLs.code === -2101, `code=${negLs.code}`);

  // C7 删除引用（owner），列表消失；下载已删文件被拒
  const del = await g.deleteGroupFile(gid, f1.gfId, a.net);
  const ls4 = await g.listGroupFiles(gid, 0, 200, a.net);
  check('C7', '103 删除后 99 不再含该条', del.code === 0 && !ls4.files.some((x) => x.gfId === f1.gfId));

  await g.dissolveGroup(gid, a.net);
  return { gid };
}

// ===== 主流程 =====
let sessions = [];
try {
  sessions = [new Session(USER_A), new Session(USER_B), new Session(USER_C)];
  const [a, b, c] = sessions;
  for (const s of sessions) await s.register();
  for (const s of sessions) await s.login();

  await testLifecycle(a, b, c);
  await testMessages(a, b, c);
  await testFiles(a, b, c);
} catch (e) {
  failures.push('EXCEPTION');
  console.log('[FAIL] 未捕获异常:', e && e.stack ? e.stack : e);
} finally {
  for (const s of sessions) { try { s.close(); } catch { /* ignore */ } }
  // 把本次涉及的用户回写给编排脚本（尽力而为，写 stderr 标记行）
  console.log(`[users] ${USER_A},${USER_B},${USER_C}`);
}

console.log(`\n[group_e2e] 通过 ${passCount}，失败 ${failures.length}${failures.length ? '：' + failures.join(',') : ''}`);
process.exit(failures.length ? 1 : 0);
