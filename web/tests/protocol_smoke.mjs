// 前端协议层冒烟测试（无需浏览器）：用 node:crypto 交叉验证 MD5 哈希链、
// PDU 编解码往返、分帧器重组、登录/注册 caMsg 字节布局。
// 运行：node web/tests/protocol_smoke.mjs
import assert from 'node:assert';
import crypto from 'node:crypto';

const SRC = '../frontend/src/protocol';
const { encodePDU, decodePDU, HEADER_SIZE } = await import(`${SRC}/pdu.js`);
const { Framer } = await import(`${SRC}/framer.js`);
const fields = await import(`${SRC}/fields.js`);
const account = await import(`${SRC}/commands/account.js`);
const chat = await import(`${SRC}/commands/chat.js`);
const friend = await import(`${SRC}/commands/friend.js`);
const drive = await import(`${SRC}/commands/drive.js`);
const { T } = await import(`${SRC}/msgtypes.js`);

let pass = 0;
function ok(name) { pass++; console.log(`  ok  ${name}`); }

// 1) MD5 哈希链：js-md5(utf8 bytes) 必须等于 node crypto 对相同 UTF-8 字节的 MD5 hex。
for (const pwd of ['123456', 'password', '中文密码测试', 'p@ss w0rd!']) {
  const expected = crypto.createHash('md5').update(Buffer.from(pwd, 'utf8')).digest('hex');
  const got = account.passwordHashForProtocol(pwd);
  assert.strictEqual(got, expected, `MD5 不符: ${pwd}`);
  assert.strictEqual(got.length, 32);
}
// 已知向量兜底
assert.strictEqual(account.passwordHashForProtocol('123456'), 'e10adc3949ba59abbe56e057f20f883e');
ok('密码 MD5 哈希链与 node:crypto 一致（含中文/符号/已知向量）');

// 2) PDU 编解码往返。
{
  const caData = new Uint8Array(64);
  fields.writeFixedStringNul(caData, 0, 32, 'alice');
  fields.writeExactBytes(caData, 32, 32, fields.textEncoder.encode('0'.repeat(32)));
  const msg = fields.concatBytes('1234', new Uint8Array(1), 'a'.repeat(32));
  const buf = encodePDU(T.LOGIN_REQUEST, caData, msg);
  const dv = new DataView(buf);
  assert.strictEqual(dv.getUint32(0, true), HEADER_SIZE + msg.length); // uiTotalLen
  assert.strictEqual(dv.getUint32(4, true), msg.length);                // uiMsgLen
  assert.strictEqual(dv.getUint32(8, true), T.LOGIN_REQUEST);           // uiType
  const dec = decodePDU(buf);
  assert.strictEqual(dec.uiType, T.LOGIN_REQUEST);
  assert.strictEqual(fields.readFixedString(dec.caData, 0, 32), 'alice');
  assert.ok(Buffer.from(dec.caMsg).equals(Buffer.from(msg)));
  ok('PDU 头部字段与编解码往返一致');
}

// 3) 登录 caMsg 布局：验证码 + NUL + 32B clientId（复刻 client.cpp on_login_PB_clicked）。
{
  const captcha = '9527';
  const clientId = 'b'.repeat(32);
  const msg = fields.concatBytes(fields.textEncoder.encode(captcha), new Uint8Array(1), fields.textEncoder.encode(clientId));
  assert.strictEqual(msg.length, captcha.length + 1 + 32);
  assert.strictEqual(msg[captcha.length], 0); // NUL 分隔
  assert.strictEqual(fields.readBytesString(msg, 0, captcha.length), '9527');
  assert.strictEqual(fields.readBytesString(msg, captcha.length + 1, 32), clientId);
  ok('登录 caMsg 布局：验证码+NUL+32B clientId');
}

// 4) 注册 caMsg 布局：验证码 + NUL（无 clientId）。
{
  const captcha = 'abcd';
  const msg = fields.concatBytes(fields.textEncoder.encode(captcha), new Uint8Array(1));
  assert.strictEqual(msg.length, captcha.length + 1);
  assert.strictEqual(msg[captcha.length], 0);
  ok('注册 caMsg 布局：验证码+NUL');
}

// 5) 分帧器：多帧拼接后按任意块大小喂入，须原样、按序、完整恢复。
{
  const frames = [
    encodePDU(T.LOGIN_RESPOND, new Uint8Array(64), new Uint8Array(0)),
    encodePDU(T.CAPTCHA_RESPOND, (() => { const c = new Uint8Array(64); c[0] = 0; return c; })(), fields.textEncoder.encode('9527')),
    encodePDU(T.CHAT_RESPOND, new Uint8Array(64), fields.textEncoder.encode('hello world')),
  ];
  const stream = Buffer.concat(frames.map((f) => Buffer.from(f)));
  const framer = new Framer();
  const recovered = [];
  // 以 7 字节这种“恶意”小块喂入，强制跨帧/半包处理。
  for (let i = 0; i < stream.length; i += 7) {
    for (const f of framer.push(new Uint8Array(stream.subarray(i, i + 7)))) {
      recovered.push(decodePDU(f.buffer.slice(f.byteOffset, f.byteOffset + f.byteLength)));
    }
  }
  assert.strictEqual(recovered.length, frames.length);
  assert.deepStrictEqual(recovered.map((r) => r.uiType), [T.LOGIN_RESPOND, T.CAPTCHA_RESPOND, T.CHAT_RESPOND]);
  assert.strictEqual(fields.readFixedString(recovered[2].caMsg, 0, recovered[2].uiMsgLen), 'hello world');
  ok('分帧器按 uiTotalLen 跨小块重组多帧');
}

// 6) 用户名字节长度校验（≤31 字节，中文按字节计）。
{
  assert.strictEqual(account.validateCredentials('a'.repeat(31), 'x'), null);
  assert.ok(account.validateCredentials('a'.repeat(32), 'x'));      // 32 字节超限
  assert.ok(account.validateCredentials('中'.repeat(11), 'x'));      // 11*3=33 字节超限
  assert.strictEqual(account.validateCredentials('中'.repeat(10), 'x'), null); // 30 字节 ok
  ok('用户名字节长度校验（中文按 UTF-8 字节计）');
}

// 7) Msg 记录布局与往返（对齐 C++ sizeof(Msg)=40、正文@38、totalLen=41+正文字节）。
{
  const text = '你好 hello';
  const body = fields.textEncoder.encode(text);
  const rec = chat.buildMsgRecord(text, '2026-10-24 21:30:00');
  const dv = new DataView(rec.buffer);
  assert.strictEqual(rec.length, 41 + body.length);
  assert.strictEqual(dv.getUint32(0, true), 41 + body.length);   // totalLen
  assert.strictEqual(dv.getUint32(4, true), body.length + 1);    // msgLen 含 NUL
  assert.strictEqual(fields.readFixedString(rec, 8, 30), '2026-10-24 21:30:00');
  assert.strictEqual(rec[38 + body.length], 0);                  // 正文 NUL 结尾
  assert.strictEqual(rec[rec.length - 1], 0);                    // 尾部填充为 0
  const parsed = chat.parseMsgRecords(rec, rec.length);
  assert.strictEqual(parsed.length, 1);
  assert.strictEqual(parsed[0].text, text);
  assert.strictEqual(parsed[0].time, '2026-10-24 21:30:00');
  ok('Msg 记录布局（totalLen/msgLen/Time@8/正文@38）与往返一致');
}

// 8) 多条记录串接（模拟 18 历史响应）按 totalLen 步进全部解析。
{
  const recs = ['第一条', 'second message', '含NUL安全'].map((t) => chat.buildMsgRecord(t));
  const joined = fields.concatBytes(...recs);
  const parsed = chat.parseMsgRecords(joined, joined.length);
  assert.deepStrictEqual(parsed.map((p) => p.text), ['第一条', 'second message', '含NUL安全']);
  // 截断尾部 3 字节（模拟半包/畸形）不得抛出，且仍解析出前两条。
  const truncated = joined.slice(0, joined.length - 3);
  const parsed2 = chat.parseMsgRecords(truncated, truncated.length);
  assert.ok(parsed2.length >= 2);
  ok('多记录历史解析（含畸形截断容错）');
}

// 9) 私聊正文长度上限 109 字节（桌面端 char[110] 兼容），超限抛错、边界内放行。
{
  assert.strictEqual(chat.MAX_CHAT_BODY_BYTES, 109);
  // 未连接时 sendChat 会在长度校验后抛"未连接"——超限必须先抛长度错误。
  assert.throws(() => chat.sendChat('a', 'b', 'x'.repeat(110)), /过长/);
  assert.throws(() => chat.sendChat('a', 'b', '中'.repeat(37)), /过长/); // 37*3=111 > 109
  assert.throws(() => chat.sendChat('a', 'b', ''), /不能为空/);
  ok('私聊正文 109 字节上限与空消息拦截');
}

// 10) 名字数组解析（在线列表 8 / 好友列表 12 的 N×32 布局）。
{
  const one = new Uint8Array(32); fields.writeFixedStringNul(one, 0, 32, 'alice');
  const two = new Uint8Array(32); fields.writeFixedStringNul(two, 0, 32, '张三');
  const arr = fields.concatBytes(one, two);
  assert.deepStrictEqual(friend.parseNameArray(arr, arr.length), ['alice', '张三']);
  ok('N×32 定长用户名数组解析（含中文）');
}

// ===== M3：云盘/传输帧布局（drive.js）=====

// 11) isValidFileName 镜像桌面端（trim/字节限长/禁用字符/结尾规则）。
{
  const v = drive.isValidFileName;
  assert.ok(v('a.txt') && v('中文文件.bin') && v('a'.repeat(31)));
  assert.ok(!v('') && !v(' x') && !v('x ') && !v('.') && !v('..'));
  assert.ok(!v('a/b') && !v('a\\b') && !v('a<b') && !v('a|b') && !v('a?*b'));
  assert.ok(!v('x.') && !v('a\x01b') && !v('a'.repeat(32)) && !v('中'.repeat(11)));
  ok('isValidFileName 与桌面端规则一致（31 字节/禁用字符/结尾）');
}

// 12) 分片大小 clamp(ceil(size/1024), 64KiB, 1MiB)。
{
  assert.strictEqual(drive.chunkSizeFor(0), 64 * 1024);
  assert.strictEqual(drive.chunkSizeFor(1024 * 1024), 64 * 1024);          // 1KiB→clamp 下限
  assert.strictEqual(drive.chunkSizeFor(100 * 1024 * 1024), 102400);       // 100MiB/1024
  assert.strictEqual(drive.chunkSizeFor(2 * 1024 * 1024 * 1024), 1024 * 1024); // 2MiB→clamp 上限
  ok('chunkSizeFor 三段 clamp 正确');
}

// 13) 97 帧布局 + 98 解析往返（偏移对齐 Server/protocol.h FASTCHECK_*）。
{
  const sha = 'ab'.repeat(32), reqId = 'r0'.repeat(16);
  const f = drive.buildFastcheckFrame({ shaHex: sha, name: 'f.bin', dirAbsPath: './filesys/u', fileSize: 1234n, reqId });
  assert.strictEqual(f.type, T.UPLOADFILE_FASTCHECK_REQUEST);
  const d = new DataView(f.caData.buffer);
  assert.strictEqual(d.getUint32(0, true), 1);
  assert.strictEqual(d.getUint32(4, true), 0x4643484b);
  assert.strictEqual(d.getBigInt64(8, true), 1234n);
  assert.strictEqual(d.getUint32(16, true), 64);
  assert.strictEqual(d.getUint32(20, true), 5);           // 'f.bin'
  assert.strictEqual(d.getUint32(24, true), 11);          // './filesys/u'
  assert.strictEqual(d.getUint32(28, true), 32);          // reqId
  assert.ok(Buffer.from(f.msg).equals(Buffer.concat([Buffer.from(sha), Buffer.from('f.bin'), Buffer.from('./filesys/u'), Buffer.from(reqId)])));
  // 构造 98 响应并解析
  const cd = new Uint8Array(64); const dv2 = new DataView(cd.buffer);
  cd[0] = 1; cd[1] = 1; dv2.setUint32(2, 1, true);
  dv2.setBigInt64(8, 77n, true); dv2.setBigInt64(16, 99n, true); dv2.setBigInt64(24, 0n, true);
  dv2.setUint32(32, 6, true);
  const msg = fields.concatBytes('f2.bin', reqId);
  const r98 = drive.parseFastcheckResponse({ caData: cd, caMsg: msg, uiMsgLen: msg.length });
  assert.strictEqual(r98.ret, true); assert.strictEqual(r98.hit, true);
  assert.strictEqual(r98.fileId, 77n); assert.strictEqual(r98.sessionId, 99n);
  assert.strictEqual(r98.finalName, 'f2.bin'); // nameLen=6 恰好覆盖全名
  assert.strictEqual(r98.reqIdEcho, reqId);
  ok('97 帧布局与 98 解析（含 reqId 回带归属）');
}

// 14) 29 帧布局 + 30 解析（RSUM 魔数、size@32、tid@48；caMsg=目录+NUL）。
{
  const f = drive.buildUploadInitFrame({ name: 'f.bin', fileSize: 1234n, tid: 5n, dirAbsPath: './filesys/u' });
  const d = new DataView(f.caData.buffer);
  assert.strictEqual(fields.readFixedString(f.caData, 0, 32), 'f.bin');
  assert.strictEqual(d.getBigInt64(32, true), 1234n);
  assert.strictEqual(d.getUint32(40, true), 0x5253554d);
  assert.strictEqual(d.getBigUint64(48, true), 5n);
  assert.ok(Buffer.from(f.msg).equals(Buffer.from('./filesys/u\0')));
  const cd = new Uint8Array(64); const dv2 = new DataView(cd.buffer);
  cd[0] = 1; dv2.setBigInt64(32, 64n, true); dv2.setUint32(40, 0x5253554d, true); dv2.setBigUint64(48, 5n, true);
  const r = drive.parseUploadInitResponse({ caData: cd });
  assert.strictEqual(r.ret, true); assert.strictEqual(r.resumeOffset, 64n); assert.strictEqual(r.tid, 5n);
  ok('29 帧布局与 30 解析（RSUM/续传偏移/tid）');
}

// 15) 31 帧布局 + 32 解析（绝对偏移@0、next@8、complete@16、tid@48）。
{
  const chunk = new Uint8Array([9, 8, 7]);
  const f = drive.buildUploadChunkFrame({ offset: 65536n, tid: 5n, chunk });
  const d = new DataView(f.caData.buffer);
  assert.strictEqual(d.getBigInt64(0, true), 65536n);
  assert.strictEqual(d.getUint32(40, true), 0x5253554d);
  assert.strictEqual(d.getBigUint64(48, true), 5n);
  assert.ok(Buffer.from(f.msg).equals(Buffer.from(chunk)));
  const cd = new Uint8Array(64); const dv2 = new DataView(cd.buffer);
  cd[0] = 1; dv2.setBigInt64(8, 65539n, true); cd[16] = 1; dv2.setUint32(40, 0x5253554d, true); dv2.setBigUint64(48, 5n, true);
  const a = drive.parseUploadAck({ caData: cd });
  assert.strictEqual(a.ret, true); assert.strictEqual(a.nextOffset, 65539n);
  assert.strictEqual(a.complete, true); assert.strictEqual(a.tid, 5n);
  ok('31 帧布局与 32 解析（停等确认字段）');
}

// 16) 40 帧布局 + 41 解析（fileSize 在非对齐偏移 1！tid@16、accepted@24、名@32）。
{
  const f = drive.buildDownloadFrame({ name: 'f.bin', resumeOffset: 100n, tid: 6n, serverAbsPath: './filesys/u/f.bin' });
  const d = new DataView(f.caData.buffer);
  assert.strictEqual(fields.readFixedString(f.caData, 0, 32), 'f.bin');
  assert.strictEqual(d.getBigInt64(32, true), 100n);
  assert.strictEqual(d.getBigUint64(48, true), 6n);
  assert.ok(Buffer.from(f.msg).equals(Buffer.from('./filesys/u/f.bin\0')));
  const cd = new Uint8Array(64); const dv2 = new DataView(cd.buffer);
  cd[0] = 1;
  dv2.setBigInt64(1, 1234n, true);            // 非对齐 i64@1
  dv2.setBigUint64(16, 6n, true);
  dv2.setBigInt64(24, 100n, true);
  fields.writeFixedStringNul(cd, 32, 32, 'f.bin');
  const r = drive.parseDownloadResponse({ caData: cd });
  assert.strictEqual(r.ret, true); assert.strictEqual(r.fileSize, 1234n);
  assert.strictEqual(r.tid, 6n); assert.strictEqual(r.accepted, 100n); assert.strictEqual(r.fileName, 'f.bin');
  ok('40 帧布局与 41 解析（非对齐 fileSize@1、tid@16）');
}

// 17) FileInfo 数组解析（36B 步长；0=目录 1=文件；名止于 NUL）。
{
  const mk = (name, type) => {
    const e = new Uint8Array(36);
    fields.writeFixedStringNul(e, 0, 32, name);
    new DataView(e.buffer).setUint32(32, type, true);
    return e;
  };
  const arr = fields.concatBytes(mk('dirA', 0), mk('文件.bin', 1));
  const parsed = drive.parseFileInfoArray(arr, arr.length);
  assert.deepStrictEqual(parsed, [{ name: 'dirA', isDir: true }, { name: '文件.bin', isDir: false }]);
  ok('FileInfo 36B 数组解析（0=目录/1=文件，与 protocol.h 注释相反、以服务端实现为准）');
}

// 18) 107/108 帧布局。
{
  const f = drive.buildAbortFrame(7n);
  assert.strictEqual(new DataView(f.caData.buffer).getBigUint64(48, true), 7n);
  assert.strictEqual(f.msg, null);
  const cd = new Uint8Array(64); cd[0] = 1; new DataView(cd.buffer).setBigUint64(48, 7n, true);
  const r = drive.parseAbortResponse({ caData: cd });
  assert.strictEqual(r.ret, true); assert.strictEqual(r.tid, 7n);
  ok('107/108 任务释放帧布局');
}

console.log(`\n[protocol_smoke] 全部通过：${pass} 组断言`);
