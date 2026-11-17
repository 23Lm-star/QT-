// 好友域命令（5-14, 46-47）：请求构造与响应/push 解析。
// 语义对齐 Server/msghandler.cpp（finduser/onlineuser/addfriend/addfriendFin/
// flushfriend/deletefriend/getuserinformation）与 Client/friend.cpp、onlineuser.cpp。
//
// 重要：加好友（9）没有可靠直回响应——目标在线时服务端只 push 9 给对方，
// 同意后的结果 10 会 push 给双方；仅 ret≠1（如 -2 不在线）才直回 10 给申请人。
// 因此 9/10 一律走 push 事件（NetClient.onPush），不做请求-响应匹配。
import { T } from '../msgtypes.js';
import { getNet } from '../net.js';
import {
  writeFixedStringNul, readFixedString, readI32, readU32,
} from '../fields.js';

// 解析 N×32 字节定长用户名数组（在线列表 8 / 好友列表 12 共用）。
export function parseNameArray(caMsg, uiMsgLen) {
  const n = Math.floor(uiMsgLen / 32);
  const names = [];
  for (let i = 0; i < n; i++) {
    names.push(readFixedString(caMsg, i * 32, 32));
  }
  return names;
}

function twoNameCaData(first, second) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, first);
  writeFixedStringNul(caData, 32, 32, second);
  return caData;
}

// REQ-PAGE-FETCH-001：所有用户列表（7→8，原"在线用户"）。
// 响应载荷为 N×33 字节条目：名字[32] + online(u8)；含离线用户，服务端已排除自己与好友。
export function parseUserEntries(caMsg, uiMsgLen) {
  const n = Math.floor(uiMsgLen / 33);
  const entries = [];
  for (let i = 0; i < n; i++) {
    const base = i * 33;
    entries.push({
      name: readFixedString(caMsg, base, 32),
      online: caMsg[base + 32] === 1,
    });
  }
  return entries;
}

// net 可选：缺省用共享单例；e2e 双用户场景显式传各自会话。
// 服务端 limit=0 保持旧全量语义：这里不发分页字段，一次取回全部。
export async function fetchOnlineUsers(myName, net = getNet()) {
  const resp = await net.request(T.ONLINEUSER_REQUEST, T.ONLINEUSER_RESPOND);
  return parseUserEntries(resp.caMsg, resp.uiMsgLen).filter((e) => e.name !== myName);
}

// 好友列表（11→12）。
export async function fetchFriends(net = getNet()) {
  const resp = await net.request(T.FLUSHFRIEND_REQUEST, T.FLUSHFRIEND_RESPOND);
  return parseNameArray(resp.caMsg, resp.uiMsgLen);
}

// 发送加好友请求（9）：caData[0..31]=申请人（服务端按连接身份覆写）、[32..63]=目标。
// 发送即忘；结果经 push 10 异步到达（或对方同意后 push 10 给双方）。
export function sendAddFriendRequest(myName, target, net = getNet()) {
  net.sendFrame(T.ADDFRIEND_REQUEST, twoNameCaData(myName, target));
}

// 同意加好友（10）：caData[0..31]=同意人（自己）、[32..63]=申请人。
// 发送即忘；服务端会把结果 10 push 给双方。拒绝无协议消息（与桌面端一致），仅关 UI。
export function sendAddFriendAgree(myName, requester, net = getNet()) {
  net.sendFrame(T.ADDFRIEND_RESPOND, twoNameCaData(myName, requester));
}

// 删除好友（13→14）：caData[0..31]=被删好友、[32..64)=自己。
// 响应 caData[0..3]=标准码；成功后服务端会联动清除双方私聊记录。
export async function deleteFriend(target, myName, net = getNet()) {
  const resp = await net.request(T.DELETEFRIEND_REQUEST, T.DELETEFRIEND_RESPOND, twoNameCaData(target, myName));
  return readI32(resp.caData, 0) === 0;
}

// 查找用户（5→6）：caData[0..31]=待查用户名；响应 caData[0..3]=ret。
export async function findUser(name, net = getNet()) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, name);
  const resp = await net.request(T.FIND_USER_REQUEST, T.FIND_USER_RESPOND, caData);
  return readI32(resp.caData, 0);
}

// 获取用户信息（46→47）：caData[0..31]=用户名。
// 响应 caData[0..3]=标准码（0 找到/-2013 未找到）、[4..7]=在线态、[32..63]=用户名。
export async function getUserInfo(name, net = getNet()) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, name);
  const resp = await net.request(T.GETUSERINFORMATION_REQUEST, T.GETUSERINFORMATION_RESPOND, caData);
  return {
    code: readI32(resp.caData, 0),
    online: readI32(resp.caData, 4),
    name: readFixedString(resp.caData, 32, 32),
  };
}

// push 解析：收到 9 = 他人发来的好友申请（caData[0..31]=申请人）。
export function parseIncomingFriendRequest(pdu) {
  return { requester: readFixedString(pdu.caData, 0, 32) };
}

// push 解析：收到 10 = 加好友结果（caData[0..3]=标准码）。0 关系已建立；1 申请已登记待确认。
export function parseFriendResult(pdu) {
  return { ret: readI32(pdu.caData, 0) };
}

export function describeFriendRet(ret) {
  if (ret === 0) return '好友添加成功';
  if (ret === 1) return '好友申请已发送，等待对方确认';
  return protocolCodeText(ret);
}

// readU32 再导出，便于测试断言布局。
export { readU32 };
