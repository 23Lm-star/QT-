// 私聊域命令（15-18）：Msg 记录编解码、发送、历史拉取、push 解析。
// 布局对齐 Client/chat.cpp（发送）、Server/msghandler.cpp chat()/chatmsg()（转发与历史打包）：
//   Msg 记录：totalLen u32@0 = 40 + msgLen；msgLen u32@4 = 正文字节+1（含 NUL）；
//             Time[30]@8 = "yyyy-MM-dd HH:mm:ss"；正文@38；记录尾 2 字节填充。
//   15 请求：caData[0..31]=发送人（服务端按连接身份覆写）、[32..63]=接收人；caMsg=一条 Msg 记录。
//   服务端对 15 不回响应，仅持久化后把原帧 push 给在线接收人 → 发送即忘 + 本地回显。
//   17 请求：caData[0..31]=自己、[32..63]=对方；18 响应：caMsg=连续 Msg 记录（双向、无发送者字段）。
import { T } from '../msgtypes.js';
import { getNet } from '../net.js';
import { writeFixedStringNul, readFixedString, readU32, textEncoder, textDecoder, utf8Length } from '../fields.js';

export const MSG_STRUCT_SIZE = 40;   // sizeof(Msg)：4+4+30 → 4 对齐后 40
export const MSG_TIME_OFFSET = 8;
export const MSG_TIME_SIZE = 30;
export const MSG_BODY_OFFSET = 38;   // 柔性数组 msg 的实际偏移
// 正文上限（UTF-8 字节，不含结尾 NUL）：REQ-CLI-MSGBND-001 定为业务规则，与服务端权威校验同值。
// 旧值 109 是为了迁就桌面端 char[110] 定长接收缓冲，桌面端改为按报文实际长度取用后不再需要。
export const MAX_CHAT_BODY_BYTES = 2000;

// Qt QDateTime::currentDateTime().toString() 默认格式。
export function formatQtDateTime(d = new Date()) {
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ` +
         `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

// 构造一条 Msg 记录（含尾部 2 字节填充，与 C++ sizeof(Msg)=40 对齐行为一致）。
export function buildMsgRecord(text, timeStr = formatQtDateTime()) {
  const body = textEncoder.encode(text);
  const msgLen = body.length + 1;                 // 含结尾 NUL
  const totalLen = MSG_STRUCT_SIZE + body.length + 1;
  const rec = new Uint8Array(totalLen);           // 全零 → 填充区自然为 0
  const dv = new DataView(rec.buffer);
  dv.setUint32(0, totalLen, true);
  dv.setUint32(4, msgLen, true);
  writeFixedStringNul(rec, MSG_TIME_OFFSET, MSG_TIME_SIZE, timeStr);
  rec.set(body, MSG_BODY_OFFSET);                 // rec[38+body.length] 保持 0 = NUL
  return rec;
}

// 解析连续 Msg 记录（18 历史 / 15 单条共用；dis += totalLen 步进）。
export function parseMsgRecords(caMsg, uiMsgLen) {
  const out = [];
  let dis = 0;
  while (dis + MSG_STRUCT_SIZE <= uiMsgLen) {
    const dv = new DataView(caMsg.buffer, caMsg.byteOffset + dis, caMsg.byteLength - dis);
    const totalLen = dv.getUint32(0, true);
    const msgLen = dv.getUint32(4, true);
    if (totalLen < MSG_STRUCT_SIZE || dis + totalLen > uiMsgLen || msgLen < 1 || msgLen > totalLen) break; // 畸形即止
    const time = readFixedString(caMsg, dis + MSG_TIME_OFFSET, MSG_TIME_SIZE);
    const text = readFixedString(caMsg, dis + MSG_BODY_OFFSET, msgLen);
    out.push({ text, time });
    dis += totalLen;
  }
  return out;
}

// 发送私聊（15）：发送即忘（服务端不回响应）。正文超限抛错，由 UI 拦截提示。
// net 可选：缺省用共享单例；e2e 双用户场景显式传各自会话。
export function sendChat(from, to, text, net = getNet()) {
  if (!text) throw new Error('消息不能为空');
  if (utf8Length(text) > MAX_CHAT_BODY_BYTES) {
    throw new Error(`消息过长：${utf8Length(text)} 字节，上限 ${MAX_CHAT_BODY_BYTES} 字节`);
  }
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, from);
  writeFixedStringNul(caData, 32, 32, to);
  net.sendFrame(T.CHAT_REQUEST, caData, buildMsgRecord(text));
}

// 拉取"未投递收件箱"（17→18）：仅返回 对方→我 且 issuccess=0 的消息，取回即标记已投递。
// 在线时收到的消息（push 15）入库即 issuccess=1，不会在此重复出现。
// 记录无发送者字段，无法归属己方/对方（见变更记录 M2a）。
export async function fetchChatHistory(myName, peer, net = getNet()) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, myName);
  writeFixedStringNul(caData, 32, 32, peer);
  const resp = await net.request(T.GETCHATMSG_REQUEST, T.GETCHATMSG_RESPOND, caData);
  return parseMsgRecords(resp.caMsg, resp.uiMsgLen);
}

// push 解析：收到 15 = 实时私聊（caData[0..31]=发送人、[32..63]=接收人；caMsg=一条 Msg）。
export function parseIncomingChat(pdu) {
  const from = readFixedString(pdu.caData, 0, 32);
  const to = readFixedString(pdu.caData, 32, 32);
  const recs = parseMsgRecords(pdu.caMsg, pdu.uiMsgLen);
  const rec = recs[0] || { text: '', time: '' };
  return { from, to, text: rec.text, time: rec.time };
}

// readU32/textDecoder 再导出，便于测试断言布局。
export { readU32, textDecoder };
