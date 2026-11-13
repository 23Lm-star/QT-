// 分享域命令（33-37、52-57）：好友文件分享 + 外链分享生命周期。
// 布局逐字节对齐 Server/msghandler.cpp（sharefile/sharefilemove/createPublishLink/
// resolvePublishLink/queryFilePublishLink）与 Client/sharefile.cpp、Client/file.cpp，
// 核实结论见 docs/changes/REQ-WEB-001/记录.md 的 M5a 节。
//
// 路径约定：33 的源用"服务端路径字面量"（同 19/21/23/25/27/40）；52/56 用用户根相对路径。
import { T } from '../msgtypes.js';
import { getNet } from '../net.js';
import {
  writeFixedStringNul, readFixedString, readI32, readU32, concatBytes, textEncoder,
} from '../fields.js';

// REQ-PROTO-CODE-001：外链状态并入标准码（caData[0,4)），不再单设状态字。
export const PUBLINK_STATE_VALID = 0;
export const PUBLINK_STATE_EXPIRED = -2017;
export const PUBLINK_STATE_REVOKED = -2018;
export const PUBLINK_STATE_INVALID = -2019;
export const PUBLINK_ACTION_NEW = 'new';     // 作废旧行并重新生成
export const PUBLINK_ACTION_RENEW = 'renew'; // 续期
export const PUBLINK_PREFIX = 'cloud-disk://share/';
export const PUBLINK_SAVE_DIR = '分享链接';  // 服务端 resolve 的固定落地目录
export const PUBLINK_DEFAULT_EXPIRE_HOURS = 24;      // ShareLinkService::defaultExpireTime
export const PUBLINK_RENEW_DEFAULT_DAYS = 30;        // PUBLINK_RENEW_DEFAULT_SECONDS
export const RENEW_DAY_CHOICES = [7, 30, 90, 365];

// 36→37 与 54→55 的服务端动作含物理复制、无进度反馈；桌面端一直等，网页给长超时。
export const SHARE_LONG_TIMEOUT_MS = 120000;

function pathMsg(p) { return concatBytes(textEncoder.encode(p), new Uint8Array(1)); }

function nameField32(name) {
  const bytes = textEncoder.encode(name || '');
  if (bytes.length < 1 || bytes.length > 31) throw new Error(`用户名长度非法（1..31 字节）：${name}`);
  const out = new Uint8Array(32);
  writeFixedStringNul(out, 0, 32, name);
  return out;
}

// ===== 好友文件分享（33/34、35、36/37）=====

// 33 请求：caData[0..31]=分享人、[32..35]=接收人数 i32；caMsg=N×32B 接收人 + 源路径+NUL。
export function buildShareFileFrame({ sharerName, receivers, sourceAbsPath }) {
  if (!sourceAbsPath) throw new Error('分享源路径不能为空');
  if (!Array.isArray(receivers) || receivers.length === 0) throw new Error('至少选择一位好友');
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, sharerName || '');
  new DataView(caData.buffer).setInt32(32, receivers.length, true);
  const msg = concatBytes(
    ...receivers.map((n) => nameField32(n)),
    textEncoder.encode(sourceAbsPath),
    new Uint8Array(1),
  );
  return { type: T.SHAREFILE_REQUEST, caData, msg };
}

// 33→34：34 是空响应，只表示请求被受理（不含投递/成败信息）。
export async function sendShareInvites({ sharerName, receivers, sourceAbsPath }, net = getNet()) {
  const f = buildShareFileFrame({ sharerName, receivers, sourceAbsPath });
  await net.request(f.type, T.SHAREFILE_RESPOND, f.caData, f.msg);
}

// 35 push：caData[0..31]=分享人、caMsg=源路径+NUL。
export function parseShareInvite(pdu) {
  const sharer = readFixedString(pdu.caData, 0, 32);
  const sourceAbsPath = readFixedString(pdu.caMsg, 0, pdu.uiMsgLen);
  return { sharer, sourceAbsPath, fileName: fileNameOf(sourceAbsPath) };
}

// 36 请求：caData[0..31]=接收人（服务端按连接身份覆写）、caMsg=源路径+NUL。
export function buildShareAcceptFrame({ receiverName, sourceAbsPath }) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, receiverName || '');
  return { type: T.SHAREFILE_AGREE_RESPOND, caData, msg: pathMsg(sourceAbsPath) };
}

// 36→37：37 只有 caData[0..3]=标准码，无关联字段 → 调用方须串行接受邀请。
export function parseShareFinish(pdu) {
  return { ok: readI32(pdu.caData, 0) === 0 };
}

export async function acceptShare({ receiverName, sourceAbsPath }, net = getNet()) {
  const f = buildShareAcceptFrame({ receiverName, sourceAbsPath });
  const resp = await net.request(f.type, T.SHAREFILE_FINISH_RESPOND, f.caData, f.msg, SHARE_LONG_TIMEOUT_MS);
  return parseShareFinish(resp).ok;
}

// ===== 外链分享（52/53、54/55、56/57）=====

// 52 请求：caMsg = 相对路径+NUL + [action+NUL + [seconds]]。
export function buildCreatePublishLinkFrame({ relativePath, action = '', seconds = 0 }) {
  if (typeof relativePath !== 'string' || relativePath.trim() === '') throw new Error('分享路径不能为空');
  if (action && action !== PUBLINK_ACTION_NEW && action !== PUBLINK_ACTION_RENEW) {
    throw new Error(`未知的分享动作：${action}`);
  }
  const parts = [textEncoder.encode(relativePath), new Uint8Array(1)];
  if (action) {
    parts.push(textEncoder.encode(action), new Uint8Array(1));
    if (seconds > 0) parts.push(textEncoder.encode(String(Math.floor(seconds))));
  }
  return { type: T.CREATE_PUBLINK_REQUEST, caData: null, msg: concatBytes(...parts) };
}

// 53 响应：caData[0..3]=标准码（0/-2017/-2018）、caMsg=cloud-disk://share/<token>。
export function parseCreatePublishLinkResponse(pdu) {
  const code = readI32(pdu.caData, 0);
  return {
    ok: code === 0,
    state: code,
    link: readFixedString(pdu.caMsg, 0, pdu.uiMsgLen),
  };
}

export async function createPublishLink({ relativePath, action = '', seconds = 0 }, net = getNet()) {
  const f = buildCreatePublishLinkFrame({ relativePath, action, seconds });
  const resp = await net.request(f.type, T.CREATE_PUBLINK_RESPOND, f.caData, f.msg);
  return parseCreatePublishLinkResponse(resp);
}

// 56 请求：caData[0..31]=owner（服务端覆写）、caMsg=相对路径+NUL。
export function buildQueryPublishLinkFrame({ owner, relativePath }) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, owner || '');
  return { type: T.QUERY_FILE_PUBLINK_REQUEST, caData, msg: pathMsg(relativePath) };
}

// 57 响应：caData[0..3]=标准码（0/-2017/-2018/-2019）、caMsg=链接文本（无链接时为空）。
export function parseQueryPublishLinkResponse(pdu) {
  const code = readI32(pdu.caData, 0);
  return {
    exists: code !== PUBLINK_STATE_INVALID,
    state: code,
    link: readFixedString(pdu.caMsg, 0, pdu.uiMsgLen),
  };
}

export async function queryPublishLink({ owner, relativePath }, net = getNet()) {
  const f = buildQueryPublishLinkFrame({ owner, relativePath });
  const resp = await net.request(f.type, T.QUERY_FILE_PUBLINK_RESPOND, f.caData, f.msg);
  return parseQueryPublishLinkResponse(resp);
}

// 54 请求：caMsg=token+NUL（完整链接先剥前缀；64 位 hex token_hash 也直接支持）。
export function buildResolvePublishLinkFrame(input) {
  const token = tokenFromLink(input);
  if (!token) throw new Error('分享链接不能为空');
  return { type: T.RESOLVE_PUBLINK_REQUEST, caData: null, msg: pathMsg(token) };
}

// 55 响应：caData[0..3]=标准码（0/-2017/-2018/-2019）、caMsg={"saved_path":"分享链接/<名>"}。
export function parseResolvePublishLinkResponse(pdu) {
  const ok = readI32(pdu.caData, 0) === 0;
  let savedPath = '';
  if (ok) {
    try {
      const obj = JSON.parse(readFixedString(pdu.caMsg, 0, pdu.uiMsgLen));
      savedPath = typeof obj.saved_path === 'string' ? obj.saved_path : '';
    } catch { savedPath = ''; }
  }
  return { ok, savedPath };
}

export async function resolvePublishLink(input, net = getNet()) {
  const f = buildResolvePublishLinkFrame(input);
  const resp = await net.request(f.type, T.RESOLVE_PUBLINK_RESPOND, f.caData, f.msg, SHARE_LONG_TIMEOUT_MS);
  return parseResolvePublishLinkResponse(resp);
}

// ===== 展示与路径工具 =====

export function describePublishState(state) {
  if (state === PUBLINK_STATE_EXPIRED) return '已过期';
  if (state === PUBLINK_STATE_REVOKED) return '已撤销';
  return '有效';
}

export function tokenFromLink(input) {
  const text = (input || '').trim();
  if (!text) return '';
  return text.startsWith(PUBLINK_PREFIX) ? text.slice(PUBLINK_PREFIX.length).trim() : text;
}

export function fileNameOf(p) {
  const text = p || '';
  const idx = Math.max(text.lastIndexOf('/'), text.lastIndexOf('\\'));
  return idx >= 0 ? text.slice(idx + 1) : text;
}

// 服务端路径字面量 → 用户根相对路径（52/56 用）；不属于用户根时返回空串。
export function relativePathOf(absPath, userRoot) {
  if (!absPath || !userRoot) return '';
  const root = userRoot.replace(/\/+$/, '');
  if (absPath === root) return '';
  if (!absPath.startsWith(`${root}/`)) return '';
  return absPath.slice(root.length + 1);
}
