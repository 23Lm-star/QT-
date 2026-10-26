// 云盘域命令（19-43、58/59、97/98、107/108）：目录操作 + 上传/下载帧构造与解析。
// 布局逐字节对齐 Server/msghandler.cpp 与 Client/file.cpp、transfertask.cpp、uploader.cpp，
// 详细核实结论见 docs/changes/REQ-WEB-001/记录.md 的 M3a 节。
//
// 路径约定：19/21/23/25/27/40 发"服务端文件根字面路径"（默认 ./filesys，相对服务端 CWD），
// 38/58 发用户根相对路径。u64（size/offset/tid）一律 BigInt 上线。
import { T } from '../msgtypes.js';
import { getNet } from '../net.js';
import {
  writeFixedStringNul, readFixedString, u32ToBytes, u64ToBytes, concatBytes, textEncoder,
} from '../fields.js';

// ===== 常量（对齐 Server/protocol.h 与 Client/uploader.h）=====
export const DEFAULT_ROOT = './filesys';   // 必须与服务端 connect.config 第 3 行一致
export const FILE_INFO_SIZE = 36;          // FileInfo{caName[32],uiType u32}
export const TYPE_DIR = 0;                 // 以服务端实现为准（protocol.h 注释写反）
export const TYPE_FILE = 1;

export const UPLOAD_CHUNK_TARGET_COUNT = 1024;
export const UPLOAD_CHUNK_MIN = 64 * 1024;
export const UPLOAD_CHUNK_MAX = 1024 * 1024;

export const RSUM_MAGIC = 0x5253554d;      // "RSUM" 小端
export const FCHK_MAGIC = 0x4643484b;      // "FCHK"
export const FASTCHECK_VERSION = 1;
export const FASTCHECK_MAX_MSG_LEN = 1024;

// caData 偏移
export const UPLOAD_FILE_SIZE_OFFSET = 32; // 29 请求：i64 总大小
export const UPLOAD_RESUME_OFFSET = 32;    // 30 响应：i64 续传起点
export const UPLOAD_RESUME_MAGIC_OFFSET = 40;
export const UPLOAD_CHUNK_OFFSET = 0;      // 31 请求：i64 绝对偏移
export const UPLOAD_ACK_NEXT_OFFSET = 8;   // 32 响应：i64 期望下一偏移
export const UPLOAD_ACK_COMPLETE_OFFSET = 16;
export const TRANSFER_ID_OFFSET = 48;
export const DOWNLOAD_RESP_TRANSFER_ID_OFFSET = 16; // 仅 41
export const DOWNLOAD_RESUME_OFFSET = 32;  // 40 请求声明起点
export const DOWNLOAD_ACCEPTED_OFFSET = 24; // 41 响应采纳起点

export const FASTCHECK_VERSION_OFFSET = 0;
export const FASTCHECK_MAGIC_OFFSET = 4;
export const FASTCHECK_FILE_SIZE_OFFSET = 8;
export const FASTCHECK_SHA_LEN_OFFSET = 16;
export const FASTCHECK_NAME_LEN_OFFSET = 20;
export const FASTCHECK_DIR_LEN_OFFSET = 24;
export const FASTCHECK_REQID_LEN_OFFSET = 28;
export const FASTCHECK_SHA_HEX_LEN = 64;

export const FASTCHECK_RESP_RET_OFFSET = 0;
export const FASTCHECK_RESP_HIT_OFFSET = 1;
export const FASTCHECK_RESP_VERSION_OFFSET = 2;
export const FASTCHECK_RESP_FILE_ID_OFFSET = 8;
export const FASTCHECK_RESP_SESSION_ID_OFFSET = 16;
export const FASTCHECK_RESP_NEXT_OFFSET = 24;
export const FASTCHECK_RESP_NAME_LEN_OFFSET = 32;

export const MAX_PARALLEL_UPLOADS = 3;
export const MAX_PARALLEL_DOWNLOADS = 3;
// 目录类请求与上传共用一条连接；公网慢链路下控制帧可能排在一个分片之后，
// 默认 8s 会把正常但迟到的 24/22 当成失败（表现为跳转报错、旧列表配新路径）。
export const DRIVE_REQ_TIMEOUT_MS = 30000;

// ===== 小工具 =====
function dv(u8) { return new DataView(u8.buffer, u8.byteOffset, u8.byteLength); }
export function readI64(u8, off) { return dv(u8).getBigInt64(off, true); }
function writeI64(u8, off, v) { dv(u8).setBigInt64(off, BigInt(v), true); }

export function rootPath() {
  try { return localStorage.getItem('cloudpan.root') || DEFAULT_ROOT; }
  catch { return DEFAULT_ROOT; }
}
export function userRoot(loginName) { return `${rootPath()}/${loginName}`; }
export function joinPath(dir, name) { return name ? `${dir}/${name}` : dir; }

// 复刻 Client/file.cpp isValidFileName（UTF-8 字节限长）。
export function isValidFileName(name) {
  if (typeof name !== 'string') return false;
  const trimmed = name.trim();
  if (trimmed === '' || trimmed !== name) return false;
  if (name === '.' || name === '..') return false;
  if (textEncoder.encode(name).length > 31) return false;
  if (name.endsWith('.') || name.endsWith(' ')) return false;
  const forbidden = '<>:"/\\|?*';
  for (const ch of name) {
    const cp = ch.codePointAt(0);
    if (cp < 32 || forbidden.includes(ch)) return false;
  }
  return true;
}

// 24/59 响应：FileInfo 连续数组 → [{name,isDir}]；caName 读到首个 NUL 止。
export function parseFileInfoArray(caMsg, uiMsgLen) {
  const n = Math.floor(uiMsgLen / FILE_INFO_SIZE);
  const out = [];
  for (let i = 0; i < n; i++) {
    const base = i * FILE_INFO_SIZE;
    const name = readFixedString(caMsg, base, 32);
    const uiType = dv(caMsg).getUint32(base + 32, true);
    out.push({ name, isDir: uiType === TYPE_DIR });
  }
  return out;
}

function pathMsg(p) { return concatBytes(textEncoder.encode(p), new Uint8Array(1)); }

// ===== 目录操作（请求-响应式）=====

// 23→24 列目录（absPath 为服务端路径字面量）。
export async function listDirectory(absPath, net = getNet()) {
  const resp = await net.request(T.FLUSHFILE_REQUEST, T.FLUSHFILE_RESPOND, null, pathMsg(absPath), DRIVE_REQ_TIMEOUT_MS);
  return parseFileInfoArray(resp.caMsg, resp.uiMsgLen);
}

// 19→20 建目录。
export async function makeDirectory(parentAbsPath, dirName, net = getNet()) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, dirName);
  const resp = await net.request(T.MKDIR_REQUEST, T.MKDIR_RESPOND, caData, pathMsg(parentAbsPath), DRIVE_REQ_TIMEOUT_MS);
  return resp.caData[0] !== 0;
}

// 21→22 跳转目录：返回 {exists, path}（传文件路径时服务端回其父目录）。
export async function changeDirectory(targetAbsPath, net = getNet()) {
  const resp = await net.request(T.CHANGEDIR_REQUEST, T.CHANGEDIR_RESPOND, null, pathMsg(targetAbsPath), DRIVE_REQ_TIMEOUT_MS);
  const exists = resp.caData[0] !== 0;
  const path = exists ? readFixedString(resp.caMsg, 0, resp.uiMsgLen) : '';
  return { exists, path };
}

// 25→26 删除文件/目录（uiType: 0=目录 1=文件）。
export async function deleteEntry(absPath, uiType, net = getNet()) {
  const resp = await net.request(T.DELFILE_REQUEST, T.DELFILE_RESPOND, u32ToBytes(uiType), pathMsg(absPath), DRIVE_REQ_TIMEOUT_MS);
  return resp.caData[0] !== 0;
}

// 27→28 重命名：caMsg = uiType u32 + 目录绝对路径 + NUL。
export async function renameEntry(dirAbsPath, oldName, newName, uiType, net = getNet()) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, oldName);
  writeFixedStringNul(caData, 32, 32, newName);
  const msg = concatBytes(u32ToBytes(uiType), textEncoder.encode(dirAbsPath), new Uint8Array(1));
  const resp = await net.request(T.RENAMEFILE_REQUEST, T.RENAMEFILE_RESPOND, caData, msg, DRIVE_REQ_TIMEOUT_MS);
  return resp.caData[0] !== 0;
}

// 38→39 移动（相对用户根路径）：caMsg = srcRel\0dstRel\0。返回 {ok, finalName}。
export async function moveEntry(srcRel, dstRel, srcName, net = getNet()) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, srcName);
  const msg = concatBytes(textEncoder.encode(srcRel), new Uint8Array(1), textEncoder.encode(dstRel), new Uint8Array(1));
  const resp = await net.request(T.MOVEFILE_REQUEST, T.MOVEFILE_RESPOND, caData, msg, DRIVE_REQ_TIMEOUT_MS);
  return { ok: resp.caData[0] !== 0, finalName: readFixedString(resp.caData, 32, 32) };
}

// 58→59 列子目录（移动目标选择用；relPath 空 = 用户根）。
export async function listSubDirs(relPath, net = getNet()) {
  const resp = await net.request(T.LISTDIR_REQUEST, T.LISTDIR_RESPOND, null, pathMsg(relPath || ''), DRIVE_REQ_TIMEOUT_MS);
  return parseFileInfoArray(resp.caMsg, resp.uiMsgLen).map((e) => e.name);
}

// ===== 上传链帧构造/解析（编排状态机在 stores/transfer.js）=====

export function chunkSizeFor(fileSize) {
  const size = Number(fileSize);
  if (!Number.isFinite(size) || size <= 0) return UPLOAD_CHUNK_MIN;
  return Math.min(UPLOAD_CHUNK_MAX, Math.max(UPLOAD_CHUNK_MIN, Math.ceil(size / UPLOAD_CHUNK_TARGET_COUNT)));
}

// 97 请求帧（单飞、无 tid）：reqId 由调用方生成（32 hex），98 按回带归属。
export function buildFastcheckFrame({ shaHex, name, dirAbsPath, fileSize, reqId }) {
  const nameB = textEncoder.encode(name);
  const dirB = textEncoder.encode(dirAbsPath);
  const reqB = textEncoder.encode(reqId);
  const shaB = textEncoder.encode(shaHex);
  if (shaB.length !== FASTCHECK_SHA_HEX_LEN) throw new Error('shaHex 必须为 64 位 hex');
  if (nameB.length < 1 || nameB.length > 210) throw new Error('文件名长度非法');
  if (dirB.length < 1 || dirB.length > 450) throw new Error('目录长度非法');
  if (reqB.length < 8 || reqB.length > 96) throw new Error('reqId 长度非法（8..96）');
  const msg = concatBytes(shaB, nameB, dirB, reqB);
  if (msg.length > FASTCHECK_MAX_MSG_LEN) throw new Error('秒传预检载荷超长');
  const caData = new Uint8Array(64);
  const d = dv(caData);
  d.setUint32(FASTCHECK_VERSION_OFFSET, FASTCHECK_VERSION, true);
  d.setUint32(FASTCHECK_MAGIC_OFFSET, FCHK_MAGIC, true);
  d.setBigInt64(FASTCHECK_FILE_SIZE_OFFSET, BigInt(fileSize), true);
  d.setUint32(FASTCHECK_SHA_LEN_OFFSET, FASTCHECK_SHA_HEX_LEN, true);
  d.setUint32(FASTCHECK_NAME_LEN_OFFSET, nameB.length, true);
  d.setUint32(FASTCHECK_DIR_LEN_OFFSET, dirB.length, true);
  d.setUint32(FASTCHECK_REQID_LEN_OFFSET, reqB.length, true);
  return { type: T.UPLOADFILE_FASTCHECK_REQUEST, caData, msg };
}

// 98 响应解析；reqIdEcho 用于丢弃迟到响应（nameLen 之后的剩余字节）。
export function parseFastcheckResponse(pdu) {
  const d = dv(pdu.caData);
  const nameLen = d.getUint32(FASTCHECK_RESP_NAME_LEN_OFFSET, true);
  const msg = pdu.caMsg;
  const finalName = nameLen > 0 && nameLen <= msg.length ? readFixedString(msg, 0, nameLen) : '';
  const reqIdEcho = nameLen < msg.length
    ? new TextDecoder('utf-8').decode(msg.subarray(nameLen, msg.length))
    : '';
  return {
    ret: pdu.caData[FASTCHECK_RESP_RET_OFFSET] !== 0,
    hit: pdu.caData[FASTCHECK_RESP_HIT_OFFSET] !== 0,
    version: d.getUint32(FASTCHECK_RESP_VERSION_OFFSET, true),
    fileId: readI64(pdu.caData, FASTCHECK_RESP_FILE_ID_OFFSET),
    sessionId: readI64(pdu.caData, FASTCHECK_RESP_SESSION_ID_OFFSET),
    nextOffset: readI64(pdu.caData, FASTCHECK_RESP_NEXT_OFFSET),
    finalName,
    reqIdEcho,
  };
}

// 29 请求帧：名字 + i64 大小 + RSUM + tid；caMsg=目录绝对路径+NUL。
export function buildUploadInitFrame({ name, fileSize, tid, dirAbsPath }) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, name);
  writeI64(caData, UPLOAD_FILE_SIZE_OFFSET, fileSize);
  dv(caData).setUint32(UPLOAD_RESUME_MAGIC_OFFSET, RSUM_MAGIC, true);
  writeI64(caData, TRANSFER_ID_OFFSET, tid); // setBigInt64 写 u64 位模式
  return { type: T.UPLOADFILE_INIT_REQUEST, caData, msg: pathMsg(dirAbsPath) };
}

// 30 响应解析。
export function parseUploadInitResponse(pdu) {
  const d = dv(pdu.caData);
  const hasMagic = d.getUint32(UPLOAD_RESUME_MAGIC_OFFSET, true) === RSUM_MAGIC;
  return {
    ret: pdu.caData[0] !== 0,
    resumeOffset: hasMagic ? readI64(pdu.caData, UPLOAD_RESUME_OFFSET) : 0n,
    tid: d.getBigUint64(TRANSFER_ID_OFFSET, true),
  };
}

// 31 请求帧：绝对偏移 + RSUM + tid + 块正文。
export function buildUploadChunkFrame({ offset, tid, chunk }) {
  const caData = new Uint8Array(64);
  writeI64(caData, UPLOAD_CHUNK_OFFSET, offset);
  dv(caData).setUint32(UPLOAD_RESUME_MAGIC_OFFSET, RSUM_MAGIC, true);
  writeI64(caData, TRANSFER_ID_OFFSET, tid);
  return { type: T.UPLOADFILE_DATA_REQUEST, caData, msg: chunk };
}

// 32 响应解析。
export function parseUploadAck(pdu) {
  const d = dv(pdu.caData);
  const hasMagic = d.getUint32(UPLOAD_RESUME_MAGIC_OFFSET, true) === RSUM_MAGIC;
  return {
    ret: pdu.caData[0] !== 0,
    nextOffset: hasMagic ? readI64(pdu.caData, UPLOAD_ACK_NEXT_OFFSET) : 0n,
    complete: hasMagic ? pdu.caData[UPLOAD_ACK_COMPLETE_OFFSET] !== 0 : false,
    tid: d.getBigUint64(TRANSFER_ID_OFFSET, true),
  };
}

// 107 请求帧（暂停/取消共用）与 108 解析。
export function buildAbortFrame(tid) {
  const caData = new Uint8Array(64);
  writeI64(caData, TRANSFER_ID_OFFSET, tid);
  return { type: T.UPLOADFILE_ABORT_REQUEST, caData, msg: null };
}
export function parseAbortResponse(pdu) {
  return { ret: pdu.caData[0] !== 0, tid: dv(pdu.caData).getBigUint64(TRANSFER_ID_OFFSET, true) };
}

// ===== 下载链 =====

// 40 请求帧：显示名 + 声明续传起点 + tid；caMsg=文件绝对路径+NUL。
export function buildDownloadFrame({ name, resumeOffset, tid, serverAbsPath }) {
  const caData = new Uint8Array(64);
  writeFixedStringNul(caData, 0, 32, name);
  writeI64(caData, DOWNLOAD_RESUME_OFFSET, resumeOffset);
  writeI64(caData, TRANSFER_ID_OFFSET, tid);
  return { type: T.DOWNLOADFILE_REQUEST, caData, msg: pathMsg(serverAbsPath) };
}

// 41 响应解析：ret bool@0、fileSize i64@1（非对齐）、tid@16、accepted@24、文件名@32..62。
export function parseDownloadResponse(pdu) {
  const d = dv(pdu.caData);
  return {
    ret: pdu.caData[0] !== 0,
    fileSize: readI64(pdu.caData, 1),
    tid: d.getBigUint64(DOWNLOAD_RESP_TRANSFER_ID_OFFSET, true),
    accepted: readI64(pdu.caData, DOWNLOAD_ACCEPTED_OFFSET),
    fileName: readFixedString(pdu.caData, 32, 32),
  };
}

// 42 数据块：tid@48 + 正文。
export function parseDownloadData(pdu) {
  return { tid: dv(pdu.caData).getBigUint64(TRANSFER_ID_OFFSET, true), chunk: pdu.caMsg };
}

// 43 完成：success bool@0 + tid@48。
export function parseDownloadFinish(pdu) {
  return { success: pdu.caData[0] !== 0, tid: dv(pdu.caData).getBigUint64(TRANSFER_ID_OFFSET, true) };
}

// 生成 32 位 hex 请求标识（等价桌面端 QUuid rfc4122 hex 的长度与字符集）。
export function newReqId() {
  const b = new Uint8Array(16);
  crypto.getRandomValues(b);
  return Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
}

// u64ToBytes 再导出（测试用）。
export { u64ToBytes };
