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

console.log(`\n[protocol_smoke] 全部通过：${pass} 组断言`);
