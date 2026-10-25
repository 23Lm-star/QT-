// M3 端到端：单用户经 WebSocket 网关对真实 Server.exe 验证云盘全部命令。
// 覆盖：23/24 列表、19/20 mkdir（含重名负例）、21/22 chdir（含不存在与越权负例）、
// 97/98 秒传预检与命中、29/30 初始化、31/32 停等分片（2 块与 80 块）、
// 107/108 暂停/取消、上传会话内续传（staging 派生）、40-43 下载往返 SHA 一致、
// 下载会话内续传（accepted 核对）、27/28 重命名（含占用负例）、58/59 子目录、
// 38/39 移动（相对路径）、25/26 删除（文件与目录子树）。
// 前置：dist/Server.exe 监听 5000，网关监听 8080（run_m3_e2e.py 编排与清理）。
// 运行：node web/tests/drive_e2e.mjs
import assert from 'node:assert';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const SRC = '../frontend/src/protocol';
const { NetClient } = await import(`${SRC}/client.js`);
const account = await import(`${SRC}/commands/account.js`);
const drive = await import(`${SRC}/commands/drive.js`);
const { T } = await import(`${SRC}/msgtypes.js`);

const GW_URL = process.env.GW_URL || 'ws://127.0.0.1:8080/ws';
const PASSWORD = '123456';
const ts = Math.floor(Date.now() / 1000);
const USER = process.env.E2E_USER || `webtd_${ts}`;
const ROOT = './filesys'; // 与 dist/Server.exe 内嵌配置第 3 行一致
const CID = 'd0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0';

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

// 上传编排（镜像 stores/transfer.js 状态机）：97 单飞 → 29/30 → 31/32 停等；
// abortAfterBytes 非空时在达到该偏移后发 107 模拟暂停/取消并返回。
async function uploadFile(s, { name, dirAbs, data, skipFastcheck = false, abortAfterBytes = null }) {
  const net = s.net;
  const sha = sha256Hex(Buffer.from(data));
  if (!skipFastcheck) {
    const reqId = drive.newReqId();
    const f = drive.buildFastcheckFrame({ shaHex: sha, name, dirAbsPath: dirAbs, fileSize: BigInt(data.length), reqId });
    const w = waitForPush(net, T.UPLOADFILE_FASTCHECK_RESPOND,
      (p) => drive.parseFastcheckResponse(p).reqIdEcho === reqId, 20000, '(98)');
    net.sendFrame(f.type, f.caData, f.msg);
    const fc = drive.parseFastcheckResponse(await w);
    assert.strictEqual(fc.version, drive.FASTCHECK_VERSION, '98 版本不符');
    assert.ok(fc.ret, '97 被拒绝');
    if (fc.hit) return { hit: true, sha, finalName: fc.finalName };
  }
  const tid = allocTid();
  const initF = drive.buildUploadInitFrame({ name, fileSize: BigInt(data.length), tid, dirAbsPath: dirAbs });
  const w30 = waitForPush(net, T.UPLOADFILE_INIT_RESPOND,
    (p) => drive.parseUploadInitResponse(p).tid === tid, 15000, '(30)');
  net.sendFrame(initF.type, initF.caData, initF.msg);
  const r30 = drive.parseUploadInitResponse(await w30);
  assert.ok(r30.ret, '29 被拒绝（并发上限？）');
  let offset = Number(r30.resumeOffset);
  const resumedFrom = offset;
  const chunkSize = drive.chunkSizeFor(data.length);
  let acks = 0;
  if (data.length > 0 && offset >= data.length) return { hit: false, complete: true, resumedFrom, acks, tid, sha };
  while (offset < data.length) {
    if (abortAfterBytes != null && offset >= abortAfterBytes) {
      const af = drive.buildAbortFrame(tid);
      const w108 = waitForPush(net, T.UPLOADFILE_ABORT_RESPOND,
        (p) => drive.parseAbortResponse(p).tid === tid, 10000, '(108)');
      net.sendFrame(af.type, af.caData, af.msg);
      const r108 = drive.parseAbortResponse(await w108);
      return { hit: false, abortedAt: offset, abortRet: r108.ret, resumedFrom, acks, tid, sha };
    }
    const end = Math.min(offset + chunkSize, data.length);
    const cf = drive.buildUploadChunkFrame({ offset: BigInt(offset), tid, chunk: data.subarray(offset, end) });
    const w32 = waitForPush(net, T.UPLOADFILE_DATA_RESPOND,
      (p) => drive.parseUploadAck(p).tid === tid, 30000, '(32)');
    net.sendFrame(cf.type, cf.caData, cf.msg);
    const ack = drive.parseUploadAck(await w32);
    assert.ok(ack.ret, `32 拒绝 @${offset}`);
    const next = Number(ack.nextOffset);
    assert.ok(next >= offset, '确认偏移回退');
    offset = next;
    acks++;
    if (ack.complete) return { hit: false, complete: true, resumedFrom, acks, tid, sha };
  }
  throw new Error('EOF 未收到 complete');
}

// 下载编排：40/41 受理 → 42 聚合 → 43 完成；abortAfterBytes 时发 107 并返回已收字节。
async function downloadFile(s, { name, serverAbs, resumeOffset = 0n, abortAfterBytes = null }) {
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
  // 可取消的完成超时守卫（race 结束即 clearTimeout，避免未处理拒绝在进程退出前崩溃）
  let guardTimer = null;
  const guard = new Promise((_, rej) => {
    guardTimer = setTimeout(() => rej(new Error('等待 43 完成超时')), 90000);
  });
  guard.catch(() => {}); // 兜底：race 已由 finP 落定后守卫拒绝不致成为 unhandled rejection
  try {
    const f = drive.buildDownloadFrame({ name, resumeOffset, tid, serverAbsPath: serverAbs });
    const w41 = waitForPush(net, T.DOWNLOADFILE_RESPOND,
      (p) => drive.parseDownloadResponse(p).tid === tid, 15000, '(41)');
    net.sendFrame(f.type, f.caData, f.msg);
    const r41 = drive.parseDownloadResponse(await w41);
    assert.ok(r41.ret, '40 被拒绝');
    if (resumeOffset > 0n) assert.strictEqual(r41.accepted, resumeOffset, '续传起点未被采纳');
    if (abortAfterBytes != null) {
      const t0 = Date.now();
      const received0 = () => parts.reduce((a, b) => a + b.length, 0);
      while (received0() < abortAfterBytes && Date.now() - t0 < 20000) await sleep(20);
      assert.ok(received0() >= abortAfterBytes, '未收到足够字节即超时');
      const af = drive.buildAbortFrame(tid);
      net.sendFrame(af.type, af.caData, af.msg);
      await sleep(400); // 收敛在途 42
      clearTimeout(guardTimer);
      return { size: Number(r41.fileSize), received: Buffer.concat(parts), aborted: true };
    }
    const success = await Promise.race([finP, guard]);
    clearTimeout(guardTimer);
    return { size: Number(r41.fileSize), received: Buffer.concat(parts), success, fileName: r41.fileName };
  } catch (e) {
    clearTimeout(guardTimer);
    throw e;
  } finally { offData(); offFin(); }
}

async function main() {
  console.log(`[drive_e2e] 网关=${GW_URL} 用户=${USER} root=${ROOT}`);
  const s = new Session(USER);
  const root = `${ROOT}/${USER}`;
  const shas = [];

  await s.register();
  await s.login();
  check('登录', '用户注册并登录成功', true, USER);

  try {
    // 1) 新用户根目录为空（23/24）
    let list = await drive.listDirectory(root, s.net);
    check('列表空', '新用户根目录列表为空', list.length === 0, JSON.stringify(list));

    // 2) mkdir（19/20）+ 列表出现目录
    const okMk = await drive.makeDirectory(root, 'dirA', s.net);
    list = await drive.listDirectory(root, s.net);
    check('mkdir', '创建目录成功且列表出现（isDir=true）',
      okMk && list.some((e) => e.name === 'dirA' && e.isDir), `ok=${okMk} list=${JSON.stringify(list)}`);

    // 3) mkdir 重名 → false（负例）
    const okMk2 = await drive.makeDirectory(root, 'dirA', s.net);
    check('mkdir重名', '重名建目录返回 false', okMk2 === false, `ret=${okMk2}`);

    // 4) chdir（21/22）：存在/不存在/越权
    const cd1 = await drive.changeDirectory(`${root}/dirA`, s.net);
    const cd2 = await drive.changeDirectory(`${root}/nope`, s.net);
    const cd3 = await drive.changeDirectory(ROOT, s.net); // 存储根=用户根之外 → 拒绝
    check('chdir', '存在目录成功、不存在拒绝、越权存储根拒绝',
      cd1.exists && cd1.path === `${root}/dirA` && !cd2.exists && !cd3.exists,
      `cd1=${cd1.exists}(${cd1.path}) cd2=${cd2.exists} cd3=${cd3.exists}`);

    // 5) 小文件上传（100KiB=2×64KiB 停等）
    const small = randBuf(100 * 1024);
    shas.push(sha256Hex(small));
    const up1 = await uploadFile(s, { name: 'small.bin', dirAbs: root, data: small });
    check('上传小文件', '97 未命中→29→31×2 停等→complete', !up1.hit && up1.complete && up1.acks === 2,
      `hit=${up1.hit} acks=${up1.acks}`);
    list = await drive.listDirectory(root, s.net);
    check('上传后列表', '文件出现在列表（isDir=false）', list.some((e) => e.name === 'small.bin' && !e.isDir), '');

    // 6) 下载往返（40-43）字节一致
    const dl1 = await downloadFile(s, { name: 'small.bin', serverAbs: `${root}/small.bin` });
    check('下载往返', '字节级一致且 SHA-256 一致',
      dl1.success === true && dl1.received.equals(Buffer.from(small)),
      `size=${dl1.size} got=${dl1.received.length}`);

    // 7) 秒传命中（同内容异名，零正文）
    const up2 = await uploadFile(s, { name: 'small2.bin', dirAbs: root, data: small });
    check('秒传命中', '相同内容再上传 hit=true（零正文）', up2.hit === true, `finalName=${up2.finalName || ''}`);
    list = await drive.listDirectory(root, s.net);
    const dl2 = await downloadFile(s, { name: 'small2.bin', serverAbs: `${root}/small2.bin` });
    check('秒传内容', '秒传条目已建且下载内容与原件一致',
      list.some((e) => e.name === 'small2.bin') && dl2.received.equals(Buffer.from(small)), '');

    // 8) 大文件多分片（5MiB → 80×64KiB）
    const big = randBuf(5 * 1024 * 1024);
    const bigSha = sha256Hex(big);
    shas.push(bigSha);
    const t0 = Date.now();
    const up3 = await uploadFile(s, { name: 'big5m.bin', dirAbs: root, data: big });
    check('上传大文件', '5MiB 80 分片停等全部确认', up3.complete && up3.acks === 80,
      `acks=${up3.acks} 用时=${Date.now() - t0}ms`);
    const dl3 = await downloadFile(s, { name: 'big5m.bin', serverAbs: `${root}/big5m.bin` });
    check('下载大文件', '5MiB 下载 SHA-256 与原件一致',
      dl3.success === true && sha256Hex(dl3.received) === bigSha && dl3.received.length === big.length,
      `len=${dl3.received.length}`);

    // 9) 上传暂停→会话内续传（107/108 + staging 派生偏移）
    const mid = randBuf(2 * 1024 * 1024);
    shas.push(sha256Hex(mid));
    const upA = await uploadFile(s, { name: 'resume2m.bin', dirAbs: root, data: mid, abortAfterBytes: 524288 });
    const upB = await uploadFile(s, { name: 'resume2m.bin', dirAbs: root, data: mid, skipFastcheck: true });
    check('上传续传', '107 暂停于 524288→29 续传 resumeOffset=524288→complete',
      upA.abortRet === true && upA.abortedAt === 524288 && upB.resumedFrom === 524288 && upB.complete === true,
      `abort@${upA.abortedAt} ret=${upA.abortRet}; resume@${upB.resumedFrom} acks=${upB.acks}`);
    const dl4 = await downloadFile(s, { name: 'resume2m.bin', serverAbs: `${root}/resume2m.bin` });
    check('续传完整性', '续传完成的文件下载 SHA 一致', sha256Hex(dl4.received) === sha256Hex(mid), '');

    // 10) 下载暂停→续传（107 停发 + 40 声明起点 + accepted 核对）
    const dlA = await downloadFile(s, { name: 'big5m.bin', serverAbs: `${root}/big5m.bin`, abortAfterBytes: 192 * 1024 });
    const R = dlA.received.length;
    const dlB = await downloadFile(s, { name: 'big5m.bin', serverAbs: `${root}/big5m.bin`, resumeOffset: BigInt(R) });
    const merged = Buffer.concat([dlA.received, dlB.received]);
    check('下载续传', '暂停后声明起点被采纳、拼接完整且 SHA 一致',
      R >= 192 * 1024 && dlB.success === true && merged.length === big.length && sha256Hex(merged) === bigSha,
      `R=${R} total=${merged.length}`);

    // 11) 上传取消：中途 107 后不续传 → 未转正文件不入列表
    const cdata = randBuf(1024 * 1024);
    const upC = await uploadFile(s, { name: 'c.bin', dirAbs: root, data: cdata, abortAfterBytes: 65536 });
    list = await drive.listDirectory(root, s.net);
    check('上传取消', '108 确认且未转正文件不出现在列表', upC.abortRet === true && !list.some((e) => e.name === 'c.bin'), '');

    // 12) 重命名（27/28）+ 目标占用负例
    const rn1 = await drive.renameEntry(root, 'small.bin', 'renamed.bin', drive.TYPE_FILE, s.net);
    const rn2 = await drive.renameEntry(root, 'renamed.bin', 'small2.bin', drive.TYPE_FILE, s.net);
    list = await drive.listDirectory(root, s.net);
    check('重命名', '成功改名、占用名拒绝、列表同步',
      rn1 === true && rn2 === false && list.some((e) => e.name === 'renamed.bin') && !list.some((e) => e.name === 'small.bin'),
      `rn1=${rn1} rn2=${rn2}`);

    // 13) listdir（58/59）
    const dirs = await drive.listSubDirs('', s.net);
    check('listSubDirs', '用户根子目录列表含 dirA', Array.isArray(dirs) && dirs.includes('dirA'), JSON.stringify(dirs));

    // 14) 移动（38/39，相对路径）
    const mv = await drive.moveEntry('renamed.bin', 'dirA', 'renamed.bin', s.net);
    const listA = await drive.listDirectory(`${root}/dirA`, s.net);
    list = await drive.listDirectory(root, s.net);
    check('移动', '文件移入 dirA：目标列表出现、原目录消失',
      mv.ok === true && mv.finalName === 'renamed.bin' && listA.some((e) => e.name === 'renamed.bin') && !list.some((e) => e.name === 'renamed.bin'),
      `ok=${mv.ok} final=${mv.finalName}`);

    // 15) 删除（25/26）：文件与目录子树。
    // 已知存量行为（服务端零改动约束下如实断言，已登记 TODO）：MKDIR 建立的目录被
    // 条目路径软删后，物理空目录残留 → flushfile 物理回退仍会列出它；对残留再删一次
    // 走物理分支（removeRecursively）即消失。桌面端同样受此影响。
    const d1 = await drive.deleteEntry(`${root}/dirA/renamed.bin`, drive.TYPE_FILE, s.net);
    const d2 = await drive.deleteEntry(`${root}/small2.bin`, drive.TYPE_FILE, s.net);
    const d3 = await drive.deleteEntry(`${root}/dirA`, drive.TYPE_DIR, s.net);
    list = await drive.listDirectory(root, s.net);
    const ghost = list.some((e) => e.name === 'dirA');
    let d3b = null;
    if (ghost) {
      d3b = await drive.deleteEntry(`${root}/dirA`, drive.TYPE_DIR, s.net); // 二次删除物理残留
      list = await drive.listDirectory(root, s.net);
    }
    check('删除', '文件删除即消失；目录软删成功（物理残留时二次删除收敛）',
      d1 && d2 && d3 && !list.some((e) => e.name === 'dirA' || e.name === 'small2.bin') && (!ghost || d3b === true),
      `d1=${d1} d2=${d2} d3=${d3} 物理残留=${ghost} d3b=${d3b}`);

    // 16) 夹具清理：删除剩余测试文件（含可能的物理残留重试），根目录应为空
    for (let round = 0; round < 3; round++) {
      list = await drive.listDirectory(root, s.net);
      if (list.length === 0) break;
      for (const e of list) {
        await drive.deleteEntry(`${root}/${e.name}`, e.isDir ? drive.TYPE_DIR : drive.TYPE_FILE, s.net);
      }
    }
    list = await drive.listDirectory(root, s.net);
    check('夹具清空', '协议删除全部测试条目后根目录为空', list.length === 0, JSON.stringify(list));
  } finally {
    s.close();
    // 夹具报告：编排脚本据此清理 file_object 行与物理对象文件。
    try {
      fs.writeFileSync(path.join(os.tmpdir(), 'm3_fixtures.json'),
        JSON.stringify({ user: USER, shas }));
    } catch (e) { console.error('[drive_e2e] 夹具报告写入失败:', e.message); }
  }

  const nfail = failures.length;
  console.log(`\nSUMMARY: ${passCount}/${passCount + nfail} PASS, ${nfail} FAIL${nfail ? ' -> ' + failures.join(',') : ''}`);
  process.exitCode = nfail === 0 ? 0 : 1;
}

main().catch((e) => { console.error('[drive_e2e] 异常:', e); process.exitCode = 2; });
