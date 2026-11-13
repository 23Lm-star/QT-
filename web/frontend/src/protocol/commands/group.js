// 群域命令（60-87 生命周期、88-96 文本、99-106 群文件）：请求构造与响应/push 解析。
// 布局逐字节对齐 Server/msghandler.cpp（groupRequest/groupFileRequest）、Server/operatedb.cpp
// （handleGroupOperation/handleGroupMessageOperation/handleGroupFileOperation）与
// Client/group.cpp、groupchat.cpp、groupfile.cpp、transfertask.cpp。
//
// 群协议与 M1-M3 的定长 caData 完全不同：所有载荷走 caMsg，以 20B 群信封开头，
// 业务字段用 groupAppendXxx 显式小端序列化（字符串 = u32 字节长 + 裸 UTF-8，无 NUL）。
//   信封：u16 version=1 @0 | u16 envelope_length=20 @2 | u64 request_id @4 | u64 group_id @12
//   请求业务字段从 offset 20 起；响应先 4B code(i32)，业务字段从 offset 24 起。
//
// 请求-响应关联：NetClient.request 仅按 respType 匹配，无法处理同类并发（群消息发送
// 与拉取会并发）。故群命令一律用 **request_id** 关联——每个请求分配单调递增 u64 写入
// 信封，响应信封回带同一 request_id；bindGroup 一次性给所有响应类型挂 onPush，按
// `respType:requestId` 投递等待者。102/106 例外：其 request_id 即 transfer_id，由
// stores/transfer.js 按 tid 路由，本模块不参与其关联（只导出帧构造/解析供其复用）。
import { T, protocolCodeText } from '../msgtypes.js';
import { getNet } from '../net.js';
import { concatBytes, textEncoder, textDecoder, utf8Length } from '../fields.js';

// ===== 信封与序列化常量 =====
export const GROUP_VERSION = 1;
export const GROUP_ENVELOPE_LEN = 20;
export const GROUP_BODY_OFFSET = 20;      // 请求业务字段起点
export const GROUP_CODE_OFFSET = 20;      // 响应 code 起点
export const GROUP_RESP_BODY_OFFSET = 24; // 响应业务字段起点

// 内容/名称字节上限（对齐服务端校验）
export const GROUP_NAME_MAX_BYTES = 128;  // 建群名
export const GROUP_MSG_MAX_BYTES = 2000;  // 群文本正文
export const GROUP_CLIENT_MSG_ID_MAX = 64;
export const GROUP_FILE_NAME_MAX_BYTES = 255; // 上传请求展示名（服务端落库再截 70）

// 群文件 PDU 偏移（相对 caMsg 起始，对齐 Server/protocol.h GROUP_FILE_*）
export const GF_LIST_OFFSET = 20, GF_LIST_LIMIT = 24;
export const GF_LIST_RESP_COUNT = 24, GF_LIST_RESP_TOTAL = 28, GF_LIST_RESP_RECORDS = 32;
export const GF_INIT_SIZE = 20;
export const GF_INIT_RESP_HIT = 24, GF_INIT_RESP_FILE_ID = 28, GF_INIT_RESP_GFID = 36;
export const GF_INIT_RESP_SESSION = 44, GF_INIT_RESP_NEXT = 52, GF_INIT_RESP_NAME_LEN = 60, GF_INIT_RESP_NAME = 64;
export const GF_DELETE_ID = 20, GF_DELETE_RESP_ID = 24;
export const GF_DOWNLOAD_ID = 20;
export const GF_DOWNLOAD_RESP_SIZE = 24, GF_DOWNLOAD_RESP_NAME_LEN = 32, GF_DOWNLOAD_RESP_NAME = 36;

// 秒传 sha 十六进制长度（与个人上传一致）
export const SHA_HEX_LEN = 64;

function v(u8) { return new DataView(u8.buffer, u8.byteOffset, u8.byteLength); }

// ===== 追加（写）原语：返回 Uint8Array 片段 =====
function bU16(val) { const b = new Uint8Array(2); v(b).setUint16(0, val & 0xffff, true); return b; }
function bU32(val) { const b = new Uint8Array(4); v(b).setUint32(0, val >>> 0, true); return b; }
function bU64(val) { const b = new Uint8Array(8); v(b).setBigUint64(0, BigInt(val), true); return b; }
// groupAppendString：u32 字节长 + 裸 UTF-8（无 NUL）
export function gStr(str) {
  const bytes = typeof str === 'string' ? textEncoder.encode(str) : str;
  return concatBytes(bU32(bytes.length), bytes);
}

// 群信封（20B）。requestId/groupId 一律 BigInt。
export function gEnvelope(requestId, groupId) {
  return concatBytes(bU16(GROUP_VERSION), bU16(GROUP_ENVELOPE_LEN), bU64(requestId), bU64(groupId));
}

// 构造一条群请求的完整 caMsg：信封 + 业务字段（从 offset 20 起）。
function buildGroupPayload(requestId, groupId, bodyBytes) {
  return concatBytes(gEnvelope(requestId, groupId), bodyBytes || new Uint8Array(0));
}

// ===== 读取原语：(u8, off) -> 值；字符串返回 {value, offset} =====
export function rU16(u8, off) { return v(u8).getUint16(off, true); }
export function rU32(u8, off) { return v(u8).getUint32(off, true); }
export function rI32(u8, off) { return v(u8).getInt32(off, true); }
export function rU64(u8, off) { return v(u8).getBigUint64(off, true); }
export function rI64(u8, off) { return v(u8).getBigInt64(off, true); }
export function readStr(u8, off, maxBytes) {
  const len = rU32(u8, off);
  if (len > u8.length - off - 4) throw new Error('群字符串长度越界');
  if (maxBytes != null && len > maxBytes) throw new Error(`群字符串超长：${len} > ${maxBytes}`);
  return { value: textDecoder.decode(u8.subarray(off + 4, off + 4 + len)), offset: off + 4 + len };
}

// 解析响应信封 + code，返回 {requestId, groupId, code, caMsg, uiMsgLen}。
// 校验 version/长度；畸形即抛错（与桌面端一致，不做部分容错）。
export function readGroupResponse(pdu) {
  const caMsg = pdu.caMsg;
  if (pdu.uiMsgLen < GROUP_ENVELOPE_LEN + 4) throw new Error('群响应长度不足');
  const version = rU16(caMsg, 0);
  const envLen = rU16(caMsg, 2);
  const requestId = rU64(caMsg, 4);
  const groupId = rU64(caMsg, 12);
  const code = rI32(caMsg, GROUP_CODE_OFFSET);
  if (version !== GROUP_VERSION || envLen !== GROUP_ENVELOPE_LEN) {
    throw new Error(`群响应信封非法：version=${version} len=${envLen}`);
  }
  return { requestId, groupId, code, caMsg, uiMsgLen: pdu.uiMsgLen };
}

// ===== 请求关联引擎 =====
let reqSeq = 0n;              // 单调递增 request_id（0 保留给服务端 push）
const waiters = new Map();    // `respType:requestId` -> {resolve, reject, timer}
const boundNets = new Set();  // 已挂响应路由的 NetClient（e2e 多会话共享本模块）

const CORRELATED_RESP = [
  T.CREATE_GROUP_RESPOND, T.INVITE_GROUP_MEMBER_RESPOND, T.ACCEPT_GROUP_INVITATION_RESPOND,
  T.REJECT_GROUP_INVITATION_RESPOND, T.LEAVE_GROUP_RESPOND, T.REMOVE_GROUP_MEMBER_RESPOND,
  T.SET_GROUP_ROLE_RESPOND, T.TRANSFER_GROUP_OWNER_RESPOND, T.DISSOLVE_GROUP_RESPOND,
  T.LIST_GROUPS_RESPOND, T.LIST_GROUP_MEMBERS_RESPOND, T.QUERY_GROUP_INVITATIONS_RESPOND,
  T.QUERY_GROUP_SUMMARY_RESPOND,
  T.GROUP_SEND_MESSAGE_RESPOND, T.GROUP_PULL_MESSAGES_RESPOND, T.GROUP_READ_CONFIRM_RESPOND,
  T.GROUP_UNREAD_SUMMARY_RESPOND,
  T.GROUP_FILE_LIST_RESPOND, T.GROUP_FILE_DELETE_RESPOND,
];

function rejectAllFor(net, err) {
  for (const [key, w] of [...waiters.entries()]) {
    if (w.net === net) {
      clearTimeout(w.timer); waiters.delete(key); w.reject(err);
    }
  }
}

// 给 net 挂一次性响应路由（每个 net 只挂一次）。
// waiters 以 `respType:requestId` 为键，reqSeq 全局单调，多会话共享本模块不串台。
// 关闭时只收敛该 net 的在途请求（rejectAllFor），不误伤其他会话。
export function bindGroup(net = getNet()) {
  if (boundNets.has(net)) return;
  boundNets.add(net);
  for (const respType of CORRELATED_RESP) {
    net.onPush(respType, (pdu) => {
      let env;
      try { env = readGroupResponse(pdu); } catch { return; }
      const key = `${respType}:${env.requestId}`;
      const w = waiters.get(key);
      if (w) { clearTimeout(w.timer); waiters.delete(key); w.resolve(pdu); }
    });
  }
  net.onClose(() => rejectAllFor(net, new Error('连接已关闭')));
}

// 发送一条群请求并等待 request_id 匹配的响应；返回 readGroupResponse 结果。
export function groupRequest(reqType, respType, groupId, bodyBytes, net = getNet(), timeoutMs = 8000) {
  bindGroup(net);
  if (!net.connected) return Promise.reject(new Error('未连接'));
  const requestId = (reqSeq += 1n);
  const payload = buildGroupPayload(requestId, groupId, bodyBytes);
  const key = `${respType}:${requestId}`;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { waiters.delete(key); reject(new Error(`群请求超时 type=${reqType}`)); }, timeoutMs);
    waiters.set(key, {
      net,
      resolve: (pdu) => { clearTimeout(timer); try { resolve(readGroupResponse(pdu)); } catch (e) { reject(e); } },
      reject: (e) => { clearTimeout(timer); reject(e); },
      timer,
    });
    try { net.sendFrame(reqType, null, payload); }
    catch (e) { clearTimeout(timer); waiters.delete(key); reject(e); }
  });
}

// ===== A. 群生命周期（60-87）=====

// 60→61 建群：S(groupName)。成功返回新 group_id。
export async function createGroup(name, net = getNet()) {
  if (!name || utf8Length(name) > GROUP_NAME_MAX_BYTES) throw new Error(`群名非法（1-${GROUP_NAME_MAX_BYTES} 字节）`);
  const r = await groupRequest(T.CREATE_GROUP_REQUEST, T.CREATE_GROUP_RESPOND, 0n, gStr(name), net);
  if (r.code === 0) r.groupId = rI64(r.caMsg, GROUP_RESP_BODY_OFFSET);
  return r;
}

// 62→63 邀请成员：S(targetName) + S(requestKey 幂等键)。成功返回 invitation_id。
export async function inviteMember(groupId, targetName, requestKey, net = getNet()) {
  const body = concatBytes(gStr(targetName), gStr(requestKey));
  const r = await groupRequest(T.INVITE_GROUP_MEMBER_REQUEST, T.INVITE_GROUP_MEMBER_RESPOND, groupId, body, net);
  if (r.code === 0) r.invitationId = rI64(r.caMsg, GROUP_RESP_BODY_OFFSET);
  return r;
}

// 64→65 接受邀请：u64 invitation_id（信封 group_id 被服务端忽略，填 0）。成功返回 group_id。
export async function acceptInvitation(invitationId, net = getNet()) {
  const r = await groupRequest(T.ACCEPT_GROUP_INVITATION_REQUEST, T.ACCEPT_GROUP_INVITATION_RESPOND, 0n, bU64(invitationId), net);
  if (r.code === 0) r.groupId = rI64(r.caMsg, GROUP_RESP_BODY_OFFSET);
  return r;
}

// 66→67 拒绝邀请：u64 invitation_id。成功响应体空。
export function rejectInvitation(invitationId, net = getNet()) {
  return groupRequest(T.REJECT_GROUP_INVITATION_REQUEST, T.REJECT_GROUP_INVITATION_RESPOND, 0n, bU64(invitationId), net);
}

// 68→69 退出群：信封 only（group_id 必填）。成功返回 group_id。
export async function leaveGroup(groupId, net = getNet()) {
  const r = await groupRequest(T.LEAVE_GROUP_REQUEST, T.LEAVE_GROUP_RESPOND, groupId, null, net);
  if (r.code === 0) r.groupId = rI64(r.caMsg, GROUP_RESP_BODY_OFFSET);
  return r;
}

// 70→71 移除成员：S(targetName)。
export async function removeMember(groupId, targetName, net = getNet()) {
  const r = await groupRequest(T.REMOVE_GROUP_MEMBER_REQUEST, T.REMOVE_GROUP_MEMBER_RESPOND, groupId, gStr(targetName), net);
  if (r.code === 0) r.groupId = rI64(r.caMsg, GROUP_RESP_BODY_OFFSET);
  return r;
}

// 72→73 设置角色：S(targetName) + S(role="admin"|"member")。
export async function setGroupRole(groupId, targetName, role, net = getNet()) {
  const body = concatBytes(gStr(targetName), gStr(role));
  const r = await groupRequest(T.SET_GROUP_ROLE_REQUEST, T.SET_GROUP_ROLE_RESPOND, groupId, body, net);
  if (r.code === 0) r.groupId = rI64(r.caMsg, GROUP_RESP_BODY_OFFSET);
  return r;
}

// 74→75 转让群主：S(targetName)。
export async function transferOwner(groupId, targetName, net = getNet()) {
  const body = concatBytes(gStr(targetName));
  const r = await groupRequest(T.TRANSFER_GROUP_OWNER_REQUEST, T.TRANSFER_GROUP_OWNER_RESPOND, groupId, body, net);
  if (r.code === 0) r.groupId = rI64(r.caMsg, GROUP_RESP_BODY_OFFSET);
  return r;
}

// 76→77 解散群：信封 only（group_id 必填）。
export async function dissolveGroup(groupId, net = getNet()) {
  const r = await groupRequest(T.DISSOLVE_GROUP_REQUEST, T.DISSOLVE_GROUP_RESPOND, groupId, null, net);
  if (r.code === 0) r.groupId = rI64(r.caMsg, GROUP_RESP_BODY_OFFSET);
  return r;
}

// 78→79 群列表：响应 u32 count + N×{u64 id, S(name), S(status), S(role), u64 version}。
export async function listGroups(net = getNet()) {
  const r = await groupRequest(T.LIST_GROUPS_REQUEST, T.LIST_GROUPS_RESPOND, 0n, null, net);
  r.groups = [];
  if (r.code === 0) {
    let off = GROUP_RESP_BODY_OFFSET;
    const count = rU32(r.caMsg, off); off += 4;
    for (let i = 0; i < count; i++) {
      const groupId = rI64(r.caMsg, off); off += 8;
      const name = readStr(r.caMsg, off); off = name.offset;
      const status = readStr(r.caMsg, off); off = status.offset;
      const role = readStr(r.caMsg, off); off = role.offset;
      const version = rU64(r.caMsg, off); off += 8;
      r.groups.push({ groupId, name: name.value, status: status.value, role: role.value, version });
    }
  }
  return r;
}

// 80→81 成员列表：响应 u32 count + N×{S(name), S(role), S(status)}。
export async function listGroupMembers(groupId, net = getNet()) {
  const r = await groupRequest(T.LIST_GROUP_MEMBERS_REQUEST, T.LIST_GROUP_MEMBERS_RESPOND, groupId, null, net);
  r.members = [];
  if (r.code === 0) {
    let off = GROUP_RESP_BODY_OFFSET;
    const count = rU32(r.caMsg, off); off += 4;
    for (let i = 0; i < count; i++) {
      const name = readStr(r.caMsg, off); off = name.offset;
      const role = readStr(r.caMsg, off); off = role.offset;
      const status = readStr(r.caMsg, off); off = status.offset;
      r.members.push({ name: name.value, role: role.value, status: status.value });
    }
  }
  return r;
}

// 82→83 邀请查询：u32 count + N×{u64 invitation_id, u64 group_id, S(group_name),
// S(inviter), S(status), u64 expires_at(秒)}。
export async function queryInvitations(net = getNet()) {
  const r = await groupRequest(T.QUERY_GROUP_INVITATIONS_REQUEST, T.QUERY_GROUP_INVITATIONS_RESPOND, 0n, null, net);
  r.invitations = [];
  if (r.code === 0) {
    let off = GROUP_RESP_BODY_OFFSET;
    const count = rU32(r.caMsg, off); off += 4;
    for (let i = 0; i < count; i++) {
      const invitationId = rI64(r.caMsg, off); off += 8;
      const groupId = rI64(r.caMsg, off); off += 8;
      const groupName = readStr(r.caMsg, off); off = groupName.offset;
      const inviter = readStr(r.caMsg, off); off = inviter.offset;
      const status = readStr(r.caMsg, off); off = status.offset;
      const expiresAt = rU64(r.caMsg, off); off += 8;
      r.invitations.push({ invitationId, groupId, groupName: groupName.value, inviter: inviter.value, status: status.value, expiresAt });
    }
  }
  return r;
}

// 86→87 群摘要：u64 group_id, S(name), S(status), u64 version, u32 member_count。
export async function queryGroupSummary(groupId, net = getNet()) {
  const r = await groupRequest(T.QUERY_GROUP_SUMMARY_REQUEST, T.QUERY_GROUP_SUMMARY_RESPOND, groupId, null, net);
  if (r.code === 0) {
    let off = GROUP_RESP_BODY_OFFSET;
    r.groupId = rI64(r.caMsg, off); off += 8;
    const name = readStr(r.caMsg, off); off = name.offset;
    const status = readStr(r.caMsg, off); off = status.offset;
    r.version = rU64(r.caMsg, off); off += 8;
    r.memberCount = rU32(r.caMsg, off);
    r.groupName = name.value; r.status = status.value;
  }
  return r;
}

// ===== B. 群文本（88-96）=====

// 91 拉取请求体：u8 mode@20 + u64 cursor@21（非对齐）+ u16 limit@29，总 31B。
export function buildGroupPullBody(mode, cursor, limit) {
  if (mode !== 0 && mode !== 1) throw new Error('pull mode 必须为 0/1');
  if (limit < 1 || limit > 100) throw new Error('pull limit 必须在 1..100');
  const body = new Uint8Array(11);
  body[0] = mode;
  v(body).setBigUint64(1, BigInt(cursor), true);
  v(body).setUint16(9, limit, true);
  return body;
}

// 群消息记录解析：{u64 seq, u64 msg_id, S(sender), u8 msg_type, u64 created_at_ms, S(content)}。
function readGroupMessageRecord(caMsg, off) {
  const seq = rI64(caMsg, off); off += 8;
  const msgId = rI64(caMsg, off); off += 8;
  const sender = readStr(caMsg, off, 64); off = sender.offset;
  const msgType = caMsg[off]; off += 1;
  const createdAt = rI64(caMsg, off); off += 8;
  const content = readStr(caMsg, off, GROUP_MSG_MAX_BYTES); off = content.offset;
  return { rec: { seq, msgId, sender: sender.value, msgType, createdAt, content: content.value }, offset: off };
}

// 88→89 发送群消息：S(clientMsgId) + S(content)。成功回显 clientMsgId + seq/msg_id/created_at_ms。
// content 上限 2000 UTF-8 字节；clientMsgId 非空 ≤64 字节（服务端校验）。
export async function sendGroupMessage(groupId, clientMsgId, content, net = getNet()) {
  if (!clientMsgId || utf8Length(clientMsgId) > GROUP_CLIENT_MSG_ID_MAX) throw new Error('clientMsgId 非法');
  if (!content) throw new Error('消息不能为空');
  if (utf8Length(content) > GROUP_MSG_MAX_BYTES) throw new Error(`消息过长：${utf8Length(content)} 字节，上限 ${GROUP_MSG_MAX_BYTES} 字节`);
  const body = concatBytes(gStr(clientMsgId), gStr(content));
  const r = await groupRequest(T.GROUP_SEND_MESSAGE_REQUEST, T.GROUP_SEND_MESSAGE_RESPOND, groupId, body, net);
  let off = GROUP_RESP_BODY_OFFSET;
  if (r.code === 0) {
    const echo = readStr(r.caMsg, off, GROUP_CLIENT_MSG_ID_MAX); off = echo.offset;
    r.clientMsgId = echo.value;
    r.seq = rI64(r.caMsg, off); off += 8;
    r.msgId = rI64(r.caMsg, off); off += 8;
    r.createdAt = rI64(r.caMsg, off);
  } else if (r.uiMsgLen > off) {
    r.clientMsgId = readStr(r.caMsg, off, GROUP_CLIENT_MSG_ID_MAX).value;
  }
  return r;
}

// 91→92 按游标拉取群消息：请求 mode/cursor/limit；响应 u8 mode, u8 has_more,
// u64 next_cursor, u64 group_last_seq, u64 unread, u32 count, records[]（mode 1 已按 seq 升序前插）。
export async function pullGroupMessages(groupId, mode, cursor, limit, net = getNet()) {
  const body = buildGroupPullBody(mode, cursor, limit);
  const r = await groupRequest(T.GROUP_PULL_MESSAGES_REQUEST, T.GROUP_PULL_MESSAGES_RESPOND, groupId, body, net);
  r.records = [];
  if (r.code === 0) {
    let off = GROUP_RESP_BODY_OFFSET;
    r.mode = r.caMsg[off]; off += 1;
    r.hasMore = r.caMsg[off] !== 0; off += 1;
    r.nextCursor = rI64(r.caMsg, off); off += 8;
    r.groupLastSeq = rI64(r.caMsg, off); off += 8;
    r.unread = rI64(r.caMsg, off); off += 8;
    const count = rU32(r.caMsg, off); off += 4;
    for (let i = 0; i < count; i++) {
      const one = readGroupMessageRecord(r.caMsg, off); off = one.offset;
      r.records.push(one.rec);
    }
  }
  return r;
}

// 93→94 已读确认：u64 read_seq；响应 u64 last_read_seq（服务端钳制后的实际持久值）。
export async function confirmGroupRead(groupId, readSeq, net = getNet()) {
  const r = await groupRequest(T.GROUP_READ_CONFIRM_REQUEST, T.GROUP_READ_CONFIRM_RESPOND, groupId, bU64(readSeq), net);
  if (r.code === 0) r.lastReadSeq = rI64(r.caMsg, GROUP_RESP_BODY_OFFSET);
  return r;
}

// 95→96 未读摘要：信封 only；响应 u32 count + N×{u64 group_id, u64 last_seq, u64 last_read_seq, u64 unread}。
export async function queryGroupUnread(net = getNet()) {
  const r = await groupRequest(T.GROUP_UNREAD_SUMMARY_REQUEST, T.GROUP_UNREAD_SUMMARY_RESPOND, 0n, null, net);
  r.items = [];
  if (r.code === 0) {
    let off = GROUP_RESP_BODY_OFFSET;
    const count = rU32(r.caMsg, off); off += 4;
    for (let i = 0; i < count; i++) {
      const groupId = rI64(r.caMsg, off); off += 8;
      const groupLastSeq = rI64(r.caMsg, off); off += 8;
      const lastReadSeq = rI64(r.caMsg, off); off += 8;
      const unread = rI64(r.caMsg, off); off += 8;
      r.items.push({ groupId, groupLastSeq, lastReadSeq, unread });
    }
  }
  return r;
}

// ===== C. 群文件（99-106）=====

// 99→100 群文件列表：u32 offset@20 + u16 limit@24（1..200）。
// 响应 u32 page_count, u32 total_active, records@32：
// {u64 group_file_id, u64 size_bytes, u64 created_at_ms, S(uploader), S(display_name)}。
export async function listGroupFiles(groupId, offset, limit, net = getNet()) {
  if (limit < 1 || limit > 200) throw new Error('群文件 limit 必须在 1..200');
  const body = new Uint8Array(6); // u32 offset + u16 limit，载荷须恰被消费（多余字节触发 -2002）
  v(body).setUint32(0, offset >>> 0, true);
  v(body).setUint16(4, limit, true);
  const r = await groupRequest(T.GROUP_FILE_LIST_REQUEST, T.GROUP_FILE_LIST_RESPOND, groupId, body, net);
  r.files = [];
  if (r.code === 0) {
    r.pageCount = rU32(r.caMsg, GF_LIST_RESP_COUNT);
    r.total = rU32(r.caMsg, GF_LIST_RESP_TOTAL);
    let off = GF_LIST_RESP_RECORDS;
    for (let i = 0; i < r.pageCount; i++) {
      const gfId = rI64(r.caMsg, off); off += 8;
      const size = rI64(r.caMsg, off); off += 8;
      const createdAt = rI64(r.caMsg, off); off += 8;
      const uploader = readStr(r.caMsg, off, 64); off = uploader.offset;
      const name = readStr(r.caMsg, off, GROUP_FILE_NAME_MAX_BYTES); off = name.offset;
      r.files.push({ gfId, size, createdAt, uploader: uploader.value, name: name.value });
    }
  }
  return r;
}

// 103→104 删除群文件引用：u64 group_file_id；响应回带 gfId（成功失败都回带）。
export async function deleteGroupFile(groupId, gfId, net = getNet()) {
  const r = await groupRequest(T.GROUP_FILE_DELETE_REQUEST, T.GROUP_FILE_DELETE_RESPOND, groupId, bU64(gfId), net);
  if (r.uiMsgLen >= GF_DELETE_RESP_ID + 8) r.ackGfId = rI64(r.caMsg, GF_DELETE_RESP_ID);
  return r;
}

// ---- 101/102 上传初始化与 105/106 下载：帧构造/解析导出给 transfer.js ----
// 这两类响应 request_id 即 transfer_id，由 transfer.js 按 tid 路由，不走 groupRequest。

// 101 请求体（caMsg，从 offset 20 起）：u64 file_size + S(sha256_hex 恰 64 位) + S(display_name)。
export function buildGroupFileInitBody(fileSize, shaHex, displayName) {
  const shaBytes = textEncoder.encode(shaHex);
  if (shaBytes.length !== SHA_HEX_LEN || !/^[0-9a-f]{64}$/.test(shaHex)) throw new Error('秒传 sha 必须为 64 位小写 hex');
  const nameBytes = textEncoder.encode(displayName);
  if (nameBytes.length < 1 || nameBytes.length > GROUP_FILE_NAME_MAX_BYTES) throw new Error('群文件名长度非法');
  if (/[\/\\\0]/.test(displayName)) throw new Error('群文件名不能含路径分隔符');
  return concatBytes(bU64(fileSize), gStr(shaBytes), gStr(nameBytes));
}

// 102 解析：code 后 hit(u8)@24、3 字节保留、i64 file_id@28、i64 gfid@36、i64 session@44、
// i64 next_offset@52、u32 name_len@60、最终展示名@64。caData[48] 回带 transfer_id。
export function parseGroupFileInitResponse(pdu) {
  const r = readGroupResponse(pdu);
  r.tid = rU64(pdu.caData, 48);
  if (r.code === 0) {
    r.hit = r.caMsg[GF_INIT_RESP_HIT] !== 0;
    r.fileId = rI64(r.caMsg, GF_INIT_RESP_FILE_ID);
    r.gfId = rI64(r.caMsg, GF_INIT_RESP_GFID);
    r.sessionId = rI64(r.caMsg, GF_INIT_RESP_SESSION);
    r.nextOffset = rI64(r.caMsg, GF_INIT_RESP_NEXT);
    const nameLen = rU32(r.caMsg, GF_INIT_RESP_NAME_LEN);
    r.finalName = nameLen > 0 ? textDecoder.decode(r.caMsg.subarray(GF_INIT_RESP_NAME, GF_INIT_RESP_NAME + nameLen)) : '';
  }
  return r;
}

// 105 请求体（caMsg，从 offset 20 起）：u64 group_file_id。
export function buildGroupFileDownloadBody(gfId) {
  return bU64(gfId);
}

// 101 完整帧（供 transfer.js 群上传复用）：信封 request_id=transfer_id，
// caData[48]=transfer_id，caMsg=信封+体。与个人 29 不同：秒传内嵌在体里，无独立 97。
export function buildGroupFileInitFrame({ groupId, fileSize, shaHex, displayName, tid }) {
  const caData = new Uint8Array(64);
  new DataView(caData.buffer).setBigUint64(TRANSFER_ID_OFF, BigInt(tid), true);
  const env = gEnvelope(BigInt(tid), BigInt(groupId));
  const msg = concatBytes(env, buildGroupFileInitBody(BigInt(fileSize), shaHex, displayName));
  return { type: T.GROUP_FILE_UPLOAD_INIT_REQUEST, caData, msg };
}

// 105 完整帧（供 transfer.js 群下载复用）：caData[48]=tid，caData[32]=续传起点，信封 request_id=tid。
export function buildGroupFileDownloadFrame({ groupId, gfId, resumeOffset, tid }) {
  const caData = new Uint8Array(64);
  const d = new DataView(caData.buffer);
  d.setBigUint64(TRANSFER_ID_OFF, BigInt(tid), true);
  d.setBigInt64(32, BigInt(resumeOffset), true);
  const env = gEnvelope(BigInt(tid), BigInt(groupId));
  const msg = concatBytes(env, buildGroupFileDownloadBody(BigInt(gfId)));
  return { type: T.GROUP_FILE_DOWNLOAD_REQUEST, caData, msg };
}

const TRANSFER_ID_OFF = 48; // 对齐 Server protocol.h TRANSFER_ID_OFFSET

// 106 解析：u64 size_bytes@24、u32 name_len@32、展示名@36。caData[48]=tid、caData[24]=accepted。
export function parseGroupFileDownloadResponse(pdu) {
  const r = readGroupResponse(pdu);
  r.tid = rU64(pdu.caData, 48);
  r.accepted = rI64(pdu.caData, 24);
  if (r.code === 0) {
    r.size = rI64(r.caMsg, GF_DOWNLOAD_RESP_SIZE);
    const nameLen = rU32(r.caMsg, GF_DOWNLOAD_RESP_NAME_LEN);
    r.displayName = nameLen > 0 ? textDecoder.decode(r.caMsg.subarray(GF_DOWNLOAD_RESP_NAME, GF_DOWNLOAD_RESP_NAME + nameLen)) : '';
  }
  return r;
}

// ===== 服务端 push 事件解析（84/85/90，request_id=0）=====

// 84 成员变化 / 85 邀请事件：固定 36B —— code@20、u64 group version@24、u32 event_type@32。
export function parseGroupMemberEvent(pdu) {
  const caMsg = pdu.caMsg;
  const groupId = rU64(caMsg, 12);
  const version = rU64(caMsg, 24);
  const eventType = rU32(caMsg, 32);
  return { groupId, version, eventType };
}

// 90 群消息事件：信封(group_id@12)+code=0@20，其后记录字段从 @24 起：
// u64 group_id, u64 seq, u64 msg_id, S(sender), u8 msg_type, u64 created_at_ms, S(content)。
export function parseGroupMessageEvent(pdu) {
  // 信封里的 group_id@12；正文首 8B 亦是 group_id（@24），记录其余字段从 @32 起复用消息记录解析。
  const envelopeGroupId = rU64(pdu.caMsg, 12);
  const bodyGroupId = rU64(pdu.caMsg, 24);
  const rec = readGroupMessageRecord(pdu.caMsg, 32).rec;
  return { groupId: envelopeGroupId || bodyGroupId, ...rec };
}

// 幂等键生成：等价桌面端 "client-<ms>" / QUuid；时间戳+随机串保证唯一。
export function newClientMsgId() {
  return `w${Date.now().toString(36)}${Math.random().toString(36).slice(2, 10)}`;
}
export function newInviteKey() {
  return `k${Date.now().toString(36)}${Math.random().toString(36).slice(2, 8)}`;
}

// 群错误码 → 中文（对齐 Client/group.cpp describe）。
// 生命周期用 -1xxx 段，群文本/群文件用 -2xxx 段（-2101 起）。
export function describeGroupCode(code) {
  switch (code) {
    case 0: return '';
    case -1001: return '未登录或会话失效';
    case -1002: return '数据库不可达，请联系服务端';
    case -1009: return '请求缺少群号';
    case -1010: return '邀请不存在';
    case -1011: case -1014: case -1017: return '群不存在';
    case -1012: return '你不是该群成员，无法查看';
    case -1018: return '邀请不存在或已被处理';
    case -1019: return '该邀请不属于你，或已被处理';
    case -1020: return '邀请已过期或群已解散';
    case -1027: return '群已解散，或你已不在群中';
    case -1028: return '只有群主或管理员才能邀请成员';
    case -1029: return '参数非法（用户名过长等）';
    case -1030: return '目标用户不存在，请核对用户名';
    case -1031: case -1032: return '对方已经是群成员';
    case -1034: return '对方已有一条待处理邀请，等待其处理';
    case -1036: return '群主不能退出群，请先转让群主或解散';
    case -1037: return '你已不在该群中';
    case -1038: return '只有群主或管理员才能移除成员';
    case -1039: return '目标不是普通成员（管理员需群主先设回 member）';
    case -1041: return '只有群主能设置角色';
    case -1042: return '目标不是活跃成员或已是该角色';
    case -1043: return '只有群主能转让群主';
    case -1044: return '新群主必须是群里的普通成员';
    case -1046: return '只有群主能解散群';
    case -1047: return '解散失败（服务端错误）';
    case -2001: return '群协议信封非法';
    case -2002: return '群请求载荷格式错误';
    case -2101: return '你不是该群成员';
    case -2102: return '群不存在或已解散';
    case -2103: return '内容或参数非法';
    case -2104: return '幂等键冲突（重复消息）';
    case -2105: return '群消息/文件写入失败';
    case -2106: return '上传并发已满或任务标识冲突';
    case -2107: return '下载并发已满';
    case -2108: return '续传起点非法';
    default: return protocolCodeText(code);
  }
}

export { bU16 as groupU16Bytes, bU32 as groupU32Bytes, bU64 as groupU64Bytes };
