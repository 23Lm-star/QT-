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
const group = await import(`${SRC}/commands/group.js`);
const share = await import(`${SRC}/commands/share.js`);
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

// 9) 私聊正文长度上限 2000 字节（REQ-CLI-MSGBND-001，与服务端权威校验同值），超限抛错、边界内放行。
{
  assert.strictEqual(chat.MAX_CHAT_BODY_BYTES, 2000);
  // 未连接时 sendChat 会在长度校验后抛"未连接"——超限必须先抛长度错误。
  assert.throws(() => chat.sendChat('a', 'b', 'x'.repeat(2001)), /过长/);
  assert.throws(() => chat.sendChat('a', 'b', '中'.repeat(667)), /过长/); // 667*3=2001 > 2000
  assert.throws(() => chat.sendChat('a', 'b', ''), /不能为空/);
  assert.throws(() => chat.sendChat('a', 'b', 'x'.repeat(2000)), /未连接/);   // 恰好 2000 放行到发送环节
  assert.throws(() => chat.sendChat('a', 'b', '中'.repeat(666)), /未连接/);   // 1998 字节放行
  ok('私聊正文 2000 字节上限与空消息拦截');
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

// ===== M4：群协议帧布局（group.js）=====

// 19) 群信封 20B 逐字节：version u16@0、envelope_length u16@2、request_id u64@4、group_id u64@12。
{
  const reqId = 0x0102030405060708n;
  const gid = 9n;
  const env = group.gEnvelope(reqId, gid);
  assert.strictEqual(env.length, 20);
  const dv = new DataView(env.buffer);
  assert.strictEqual(dv.getUint16(0, true), 1);            // version
  assert.strictEqual(dv.getUint16(2, true), 20);           // envelope_length
  assert.strictEqual(dv.getBigUint64(4, true), reqId);     // request_id
  assert.strictEqual(dv.getBigUint64(12, true), gid);      // group_id
  ok('群信封 20B 逐字节布局');
}

// 20) groupAppendString：u32 字节长（小端）+ 裸 UTF-8，无 NUL。
{
  const s = group.gStr('中文a');
  assert.strictEqual(group.rU32(s, 0), 7);                 // 中(3)+文(3)+a(1)
  assert.strictEqual(s.length, 11);                        // 4 + 7
  const read = group.readStr(s, 0);
  assert.strictEqual(read.value, '中文a');
  assert.strictEqual(read.offset, 11);
  ok('群变长字符串 u32+len 编解码往返（含中文按字节）');
}

// 21) 91 拉取请求体非对齐布局：u8 mode@0、u64 cursor@1、u16 limit@9，总 11B。
{
  const body = group.buildGroupPullBody(1, 1234567890123n, 100);
  assert.strictEqual(body.length, 11);
  assert.strictEqual(body[0], 1);
  const dv = new DataView(body.buffer, body.byteOffset, body.byteLength);
  assert.strictEqual(dv.getBigUint64(1, true), 1234567890123n); // 非对齐 cursor
  assert.strictEqual(dv.getUint16(9, true), 100);
  assert.throws(() => group.buildGroupPullBody(2, 0n, 50), /mode/);
  assert.throws(() => group.buildGroupPullBody(0, 0n, 0), /limit/);
  assert.throws(() => group.buildGroupPullBody(0, 0n, 101), /limit/);
  ok('91 拉取体：mode/cursor(非对齐)/limit 与越界校验');
}

// 22) readGroupResponse：校验信封、抽出 code(i32)、requestId/groupId。
{
  const body = fields.concatBytes(group.gEnvelope(42n, 7n), group.groupU32Bytes(-2101 >>> 0));
  const r = group.readGroupResponse({ caMsg: body, uiMsgLen: body.length });
  assert.strictEqual(r.requestId, 42n);
  assert.strictEqual(r.groupId, 7n);
  assert.strictEqual(r.code, -2101);
  assert.strictEqual(group.describeGroupCode(-2101), '你不是该群成员');
  // 非法 version 必须抛
  const bad = new Uint8Array(24); new DataView(bad.buffer).setUint16(0, 9, true);
  assert.throws(() => group.readGroupResponse({ caMsg: bad, uiMsgLen: 24 }), /信封非法/);
  ok('响应信封解析与错误码中文映射（含非法 version 抛错）');
}

// 23) 101 群文件上传初始化帧体 + 102 解析往返（hit/next/finalName/tid@caData[48]）。
{
  const sha = 'a'.repeat(64);
  const body = group.buildGroupFileInitBody(4096n, sha, '群文件.bin');
  const dvb = new DataView(body.buffer, body.byteOffset, body.byteLength);
  assert.strictEqual(dvb.getBigUint64(0, true), 4096n);
  assert.strictEqual(dvb.getUint32(8, true), 64);
  assert.strictEqual(body.length, 8 + (4 + 64) + (4 + Buffer.byteLength('群文件.bin')));
  assert.throws(() => group.buildGroupFileInitBody(1n, 'Z'.repeat(64), 'x'), /64 位小写 hex/);
  assert.throws(() => group.buildGroupFileInitBody(1n, sha, 'a/b'), /路径分隔符/);

  // 构造 102 响应：envelope(reqId=5=tid, gid=7) + code(0)@20 + hit@24 + 3零 +
  // fileId@28 + gfid@36 + session@44 + next@52 + nameLen@60 + name@64
  const name = 'final.bin';
  const msg = new Uint8Array(64 + Buffer.byteLength(name));
  const dm = new DataView(msg.buffer);
  msg.set(group.gEnvelope(5n, 7n), 0);
  dm.setUint32(20, 0, true);
  msg[24] = 0; // hit=0 需分块
  dm.setBigInt64(28, 88n, true);
  dm.setBigInt64(36, 0n, true);
  dm.setBigInt64(44, 555n, true);
  dm.setBigInt64(52, 2048n, true);
  dm.setUint32(60, Buffer.byteLength(name), true);
  msg.set(Buffer.from(name), 64);
  const cd = new Uint8Array(64); new DataView(cd.buffer).setBigUint64(48, 5n, true);
  const r = group.parseGroupFileInitResponse({ caMsg: msg, uiMsgLen: msg.length, caData: cd });
  assert.strictEqual(r.code, 0);
  assert.strictEqual(r.tid, 5n);
  assert.strictEqual(r.hit, false);
  assert.strictEqual(r.fileId, 88n);
  assert.strictEqual(r.sessionId, 555n);
  assert.strictEqual(r.nextOffset, 2048n);
  assert.strictEqual(r.finalName, name);
  ok('101/102 群文件上传初始化帧布局与解析（含秒传 hit 位）');
}

// 24) 105 下载体 + 106 解析（size@24/nameLen@32/name@36、accepted@caData[24]、tid@caData[48]）。
{
  const dlBody = group.buildGroupFileDownloadBody(123n);
  assert.strictEqual(new DataView(dlBody.buffer).getBigUint64(0, true), 123n);
  const name = 'down.bin';
  const msg = new Uint8Array(36 + Buffer.byteLength(name));
  const dm = new DataView(msg.buffer);
  msg.set(group.gEnvelope(6n, 7n), 0);
  dm.setUint32(20, 0, true);
  dm.setBigUint64(24, 999n, true);
  dm.setUint32(32, Buffer.byteLength(name), true);
  msg.set(Buffer.from(name), 36);
  const cd = new Uint8Array(64); const dcd = new DataView(cd.buffer);
  dcd.setBigInt64(24, 100n, true);  // accepted
  dcd.setBigUint64(48, 6n, true);   // tid
  const r = group.parseGroupFileDownloadResponse({ caMsg: msg, uiMsgLen: msg.length, caData: cd });
  assert.strictEqual(r.tid, 6n);
  assert.strictEqual(r.accepted, 100n);
  assert.strictEqual(r.size, 999n);
  assert.strictEqual(r.displayName, name);
  ok('105/106 群文件下载帧布局与解析（accepted/tid 走 caData）');
}

// 25) 84/85 成员/邀请事件 push：固定 36B，group_id@12、version u64@24、event_type u32@32。
{
  const msg = new Uint8Array(36);
  const dm = new DataView(msg.buffer);
  msg.set(group.gEnvelope(0n, 77n), 0);
  dm.setUint32(20, 0, true);
  dm.setBigUint64(24, 5n, true);   // group version
  dm.setUint32(32, 1, true);        // event_type 1=成员变化
  const ev = group.parseGroupMemberEvent({ caMsg: msg, uiMsgLen: 36 });
  assert.strictEqual(ev.groupId, 77n);
  assert.strictEqual(ev.version, 5n);
  assert.strictEqual(ev.eventType, 1);
  ok('84/85 群事件 push 解析（36B 定长，无字符串体）');
}

// 26) 90 群消息 push：group_id + seq + msg_id + S(sender) + u8 msg_type + u64 created_at + S(content)。
{
  const sender = 'alice', content = '大家好';
  const parts = [
    group.gEnvelope(0n, 88n),
    group.groupU32Bytes(0),                 // code@20
    group.groupU64Bytes(88n),               // group_id@24
    group.groupU64Bytes(3n),                // seq@32
    group.groupU64Bytes(1003n),             // msg_id@40
    group.gStr(sender),                     // sender@48
    new Uint8Array([0]),                    // msg_type=0 text
    group.groupU64Bytes(1700000000000n),    // created_at_ms
    group.gStr(content),                    // content
  ];
  const u8 = new Uint8Array(Buffer.concat(parts.map((p) => Buffer.from(p))));
  const ev = group.parseGroupMessageEvent({ caMsg: u8, uiMsgLen: u8.length });
  assert.strictEqual(ev.groupId, 88n);
  assert.strictEqual(ev.seq, 3n);
  assert.strictEqual(ev.msgId, 1003n);
  assert.strictEqual(ev.sender, sender);
  assert.strictEqual(ev.msgType, 0);
  assert.strictEqual(ev.createdAt, 1700000000000n);
  assert.strictEqual(ev.content, content);
  ok('90 群消息事件 push 解析（记录字段自 @32 复用消息记录读取）');
}

// 27) 发送校验：空/超长正文（>2000 字节）/非法 clientMsgId 在联网前即拒绝（async→rejected）。
{
  await assert.rejects(() => group.sendGroupMessage(1n, 'id1', ''), /不能为空/);
  await assert.rejects(() => group.sendGroupMessage(1n, 'id1', 'x'.repeat(2001)), /过长/);
  await assert.rejects(() => group.sendGroupMessage(1n, '', 'hi'), /clientMsgId/);
  ok('88 发送侧内容 2000 字节上限与空消息拦截');
}

// 28) 99/100 群文件列表记录字段布局（page_count/total + N×{u64 gfId, u64 size, u64 created, S(uploader), S(name)}）。
{
  const uploader = 'bob', name = 'f.txt';
  const rec = Buffer.concat([
    Buffer.from(group.groupU64Bytes(7n)),           // gfId
    Buffer.from(group.groupU64Bytes(1234n)),        // size
    Buffer.from(group.groupU64Bytes(1700000000n)),  // created_at_ms
    Buffer.from(group.gStr(uploader)),
    Buffer.from(group.gStr(name)),
  ]);
  const recU8 = new Uint8Array(rec);
  // 从记录区按读取原语顺序走一遍，证明布局可定位、字段可还原
  let off = 0;
  const gfId = group.rU64(recU8, off); off += 8;
  const size = group.rU64(recU8, off); off += 8;
  const created = group.rU64(recU8, off); off += 8;
  const up = group.readStr(recU8, off); off = up.offset;
  const nm = group.readStr(recU8, off);
  assert.deepStrictEqual([gfId, size, created], [7n, 1234n, 1700000000n]);
  assert.strictEqual(up.value, uploader);
  assert.strictEqual(nm.value, name);
  ok('99/100 群文件列表响应记录字段（u64×3 + S + S）顺序读取还原');
}

// 29) 33 分享请求：caData[0..31]=分享人、[32..35]=人数；caMsg=N×32B 接收人 + 源路径+NUL。
{
  const src = './filesys/alice/a.txt';
  const f = share.buildShareFileFrame({ sharerName: 'alice', receivers: ['bob', 'carol'], sourceAbsPath: src });
  assert.strictEqual(f.type, T.SHAREFILE_REQUEST);
  assert.strictEqual(fields.readFixedString(f.caData, 0, 32), 'alice');
  assert.strictEqual(new DataView(f.caData.buffer).getInt32(32, true), 2);
  assert.strictEqual(f.msg.length, 64 + Buffer.byteLength(src) + 1);
  assert.strictEqual(fields.readFixedString(f.msg, 0, 32), 'bob');
  assert.strictEqual(fields.readFixedString(f.msg, 32, 32), 'carol');
  assert.strictEqual(fields.readFixedString(f.msg, 64, f.msg.length - 64), src);
  assert.strictEqual(f.msg[f.msg.length - 1], 0);
  assert.throws(() => share.buildShareFileFrame({ sharerName: 'a', receivers: ['x'.repeat(32)], sourceAbsPath: 'p' }), /长度非法/);
  assert.throws(() => share.buildShareFileFrame({ sharerName: 'a', receivers: [], sourceAbsPath: 'p' }), /至少选择/);
  ok('33 分享请求布局：caData 分享人+人数、caMsg N×32B 接收人+源路径+NUL');
}

// 30) 35 push：caData[0..31]=分享人、caMsg=源路径+NUL → {sharer, sourceAbsPath, fileName}。
{
  const caData = new Uint8Array(64);
  fields.writeFixedStringNul(caData, 0, 32, 'bob');
  const msg = fields.concatBytes('./filesys/bob/报告.txt', new Uint8Array(1));
  const inv = share.parseShareInvite({ caData, caMsg: msg, uiMsgLen: msg.length });
  assert.deepStrictEqual(inv, { sharer: 'bob', sourceAbsPath: './filesys/bob/报告.txt', fileName: '报告.txt' });
  ok('35 分享邀请 push 解析（分享人 + 源路径 + 文件名）');
}

// 31) 36 接受请求：caData[0..31]=接收人、caMsg=源路径+NUL；37 回执 bool@0。
{
  const src = './filesys/bob/x.txt';
  const f = share.buildShareAcceptFrame({ receiverName: 'bob', sourceAbsPath: src });
  assert.strictEqual(f.type, T.SHAREFILE_AGREE_RESPOND);
  assert.strictEqual(fields.readFixedString(f.caData, 0, 32), 'bob');
  assert.strictEqual(fields.readFixedString(f.msg, 0, f.msg.length), src);
  assert.strictEqual(f.msg[f.msg.length - 1], 0);
  const ca = new Uint8Array(64); ca[0] = 1;
  assert.strictEqual(share.parseShareFinish({ caData: ca, caMsg: new Uint8Array(0), uiMsgLen: 0 }).ok, true);
  ca[0] = 0;
  assert.strictEqual(share.parseShareFinish({ caData: ca, caMsg: new Uint8Array(0), uiMsgLen: 0 }).ok, false);
  ok('36/37 接受分享请求与回执（接收人 + 源路径 / bool）');
}

// 32) 52 创建外链请求：路径+NUL + [action+NUL + [seconds]]。
{
  const base = share.buildCreatePublishLinkFrame({ relativePath: 'a.txt' });
  assert.strictEqual(base.type, T.CREATE_PUBLINK_REQUEST);
  assert.strictEqual(base.caData, null);
  assert.deepStrictEqual(Array.from(base.msg), Array.from(fields.concatBytes('a.txt', new Uint8Array(1))));

  const fresh = share.buildCreatePublishLinkFrame({ relativePath: 'dir/a.txt', action: 'new' });
  assert.deepStrictEqual(Array.from(fresh.msg),
    Array.from(fields.concatBytes('dir/a.txt', new Uint8Array(1), 'new', new Uint8Array(1))));

  const renew = share.buildCreatePublishLinkFrame({ relativePath: 'a.txt', action: 'renew', seconds: 604800 });
  assert.deepStrictEqual(Array.from(renew.msg),
    Array.from(fields.concatBytes('a.txt', new Uint8Array(1), 'renew', new Uint8Array(1), '604800')));

  assert.throws(() => share.buildCreatePublishLinkFrame({ relativePath: '' }), /不能为空/);
  assert.throws(() => share.buildCreatePublishLinkFrame({ relativePath: 'a', action: 'bogus' }), /未知/);
  ok('52 创建外链请求布局：路径+NUL+[action+NUL+[seconds]]');
}

// 33) 53 创建外链响应：ok@0、state@8、caMsg=cloud-disk://share/<token>。
{
  const caData = new Uint8Array(64);
  caData[0] = 1;
  new DataView(caData.buffer).setUint32(share.PUBLINK_STATE_OFFSET, share.PUBLINK_STATE_EXPIRED, true);
  const link = 'cloud-disk://share/AbC-_1234567890abcd';
  const msg = fields.concatBytes(link, new Uint8Array(1));
  const r = share.parseCreatePublishLinkResponse({ caData, caMsg: msg, uiMsgLen: msg.length });
  assert.deepStrictEqual(r, { ok: true, state: share.PUBLINK_STATE_EXPIRED, link });
  caData[0] = 0;
  const r0 = share.parseCreatePublishLinkResponse({ caData, caMsg: new Uint8Array(0), uiMsgLen: 0 });
  assert.deepStrictEqual([r0.ok, r0.state, r0.link], [false, share.PUBLINK_STATE_EXPIRED, '']);
  ok('53 创建外链响应：ok@0、state@8、链接文本');
}

// 34) 56/57 查询外链：owner 32B + 相对路径+NUL；响应 exists@0、state@8。
{
  const f = share.buildQueryPublishLinkFrame({ owner: 'alice', relativePath: 'dir/f.txt' });
  assert.strictEqual(f.type, T.QUERY_FILE_PUBLINK_REQUEST);
  assert.strictEqual(fields.readFixedString(f.caData, 0, 32), 'alice');
  assert.strictEqual(fields.readFixedString(f.msg, 0, f.msg.length), 'dir/f.txt');

  const caData = new Uint8Array(64);
  new DataView(caData.buffer).setUint32(share.PUBLINK_STATE_OFFSET, share.PUBLINK_STATE_REVOKED, true);
  const none = share.parseQueryPublishLinkResponse({ caData, caMsg: new Uint8Array(0), uiMsgLen: 0 });
  assert.deepStrictEqual(none, { exists: false, state: share.PUBLINK_STATE_REVOKED, link: '' });

  caData[0] = 1;
  const link = 'cloud-disk://share/' + 'f'.repeat(64);
  const msg = fields.concatBytes(link, new Uint8Array(1));
  const hit = share.parseQueryPublishLinkResponse({ caData, caMsg: msg, uiMsgLen: msg.length });
  assert.deepStrictEqual(hit, { exists: true, state: share.PUBLINK_STATE_REVOKED, link });
  ok('56/57 查询外链请求与响应（exists/state/链接文本）');
}

// 35) 54/55 外链取文件：前缀剥离 + saved_path JSON（坏 JSON 安全返回空路径）。
{
  assert.strictEqual(share.tokenFromLink('cloud-disk://share/AbC-_1'), 'AbC-_1');
  assert.strictEqual(share.tokenFromLink('  tok  '), 'tok');
  assert.strictEqual(share.tokenFromLink(''), '');
  const f = share.buildResolvePublishLinkFrame('cloud-disk://share/tok123');
  assert.strictEqual(f.type, T.RESOLVE_PUBLINK_REQUEST);
  assert.deepStrictEqual(Array.from(f.msg), Array.from(fields.concatBytes('tok123', new Uint8Array(1))));
  assert.throws(() => share.buildResolvePublishLinkFrame('   '), /不能为空/);

  const mkPdu = (ok, text) => {
    const caData = new Uint8Array(64);
    caData[0] = ok ? 1 : 0;
    const msg = fields.concatBytes(text, new Uint8Array(1));
    return { caData, caMsg: msg, uiMsgLen: msg.length };
  };
  assert.deepStrictEqual(share.parseResolvePublishLinkResponse(mkPdu(true, '{"saved_path":"分享链接/a.txt"}')),
    { ok: true, savedPath: '分享链接/a.txt' });
  assert.deepStrictEqual(share.parseResolvePublishLinkResponse(mkPdu(true, '{broken')),
    { ok: true, savedPath: '' });
  assert.deepStrictEqual(share.parseResolvePublishLinkResponse(mkPdu(false, '{"saved_path":"x"}')),
    { ok: false, savedPath: '' });
  ok('54/55 外链取文件：token 前缀剥离、saved_path JSON 解析、坏 JSON/失败安全');
}

// 36) 分享路径与状态工具。
{
  assert.strictEqual(share.relativePathOf('./filesys/alice/dir/f.txt', './filesys/alice'), 'dir/f.txt');
  assert.strictEqual(share.relativePathOf('./filesys/alice/f.txt', './filesys/alice/'), 'f.txt');
  assert.strictEqual(share.relativePathOf('./filesys/alice', './filesys/alice'), '');
  assert.strictEqual(share.relativePathOf('./filesys/alice2/f.txt', './filesys/alice'), '');
  assert.strictEqual(share.relativePathOf('', './filesys/alice'), '');
  assert.strictEqual(share.fileNameOf('./filesys/a/b.txt'), 'b.txt');
  assert.strictEqual(share.fileNameOf('b.txt'), 'b.txt');
  assert.deepStrictEqual(
    [share.PUBLINK_STATE_VALID, share.PUBLINK_STATE_EXPIRED, share.PUBLINK_STATE_REVOKED].map(share.describePublishState),
    ['有效', '已过期', '已撤销'],
  );
  ok('分享路径/状态工具：相对路径、文件名、状态文案');
}

console.log(`\n[protocol_smoke] 全部通过：${pass} 组断言`);
