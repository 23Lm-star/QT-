// REQ-AUTH-CONNECTION-001 会话域（119/120、121/122、123）：纯编解码与校验。
// 布局常量与 Server/Client protocol.h 的探针区块逐字节一致（protocol_probe.py 校验）。
// 约定：clientId=32 位小写十六进制；恢复凭据=64 位小写十六进制，仅在登录成功时经 caMsg 下发一次。
import { CA_DATA_SIZE } from './pdu.js';
import { readI32, readU64, readFixedString, textEncoder } from './fields.js';

export const CLIENT_ID_HEX_LEN = 32;
export const RESUME_TOKEN_HEX_LEN = 64;
export const SESSION_CLIENT_ID_OFFSET = 0;     // 44/50/119/121 请求 caData[0,32)=clientId
export const SESSION_RESP_CODE_OFFSET = 0;     // 120/122 响应、123 事件 caData[0,4)=int32 结果码
export const SESSION_RESP_VERSION_OFFSET = 4;  // 122/123 caData[4,12)=u64 登录版本（小端）
export const SESSION_RESP_USER_OFFSET = 32;    // 122 响应 caData[32,64)=用户名（NUL 定长槽）
export const SESSION_RESUME_MSG_LEN = RESUME_TOKEN_HEX_LEN + 1; // 121 请求 caMsg：token+NUL

const HEX32_RE = /^[0-9a-f]{32}$/;
const HEX64_RE = /^[0-9a-f]{64}$/;

export function isValidClientId(s) {
  return typeof s === 'string' && HEX32_RE.test(s);
}

export function isValidResumeToken(s) {
  return typeof s === 'string' && HEX64_RE.test(s);
}

// 44/50/119/121 请求 caData：clientId 落 [0,32)，其余清零；非法即抛，禁止半截帧。
export function buildSessionCaData(clientId) {
  if (!isValidClientId(clientId)) throw new Error('clientId 缺失或格式非法');
  const caData = new Uint8Array(CA_DATA_SIZE);
  caData.set(textEncoder.encode(clientId), SESSION_CLIENT_ID_OFFSET);
  return caData;
}

// 121 请求 caMsg：64 位十六进制 token + NUL，共 65 字节。
export function buildResumeMsg(token) {
  if (!isValidResumeToken(token)) throw new Error('恢复凭据格式非法');
  const msg = new Uint8Array(SESSION_RESUME_MSG_LEN);
  msg.set(textEncoder.encode(token), 0);
  msg[RESUME_TOKEN_HEX_LEN] = 0;
  return msg;
}

export function parseLogoutRespond(resp) {
  return { ret: readI32(resp.caData, SESSION_RESP_CODE_OFFSET) };
}

export function parseResumeRespond(resp) {
  const ret = readI32(resp.caData, SESSION_RESP_CODE_OFFSET);
  const out = { ret };
  if (ret === 0) {
    out.loginVersion = readU64(resp.caData, SESSION_RESP_VERSION_OFFSET);
    out.username = readFixedString(resp.caData, SESSION_RESP_USER_OFFSET, 32);
  }
  return out;
}

// 123 事件：caData[0,4)=原因码，[4,12)=失效版本，caMsg=可选短原因文本。
export function parseInvalidatedEvent(resp) {
  return {
    reason: readI32(resp.caData, SESSION_RESP_CODE_OFFSET),
    version: readU64(resp.caData, SESSION_RESP_VERSION_OFFSET),
    text: resp.uiMsgLen > 0 ? readFixedString(resp.caMsg, 0, resp.uiMsgLen) : '',
  };
}
