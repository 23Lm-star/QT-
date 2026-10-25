// PDU 编解码：76 字节定长头（小端）+ 柔性正文，逐字节对齐 C++ struct PDU。
//   uiTotalLen u32 @0  = 76 + uiMsgLen
//   uiMsgLen   u32 @4  = caMsg 字节数
//   uiType     u32 @8
//   caData[64]     @12
//   caMsg[]        @76
export const HEADER_SIZE = 76;
export const CA_DATA_OFFSET = 12;
export const CA_DATA_SIZE = 64;
export const CA_MSG_OFFSET = 76;

function toU8(x) {
  if (x == null) return new Uint8Array(0);
  if (x instanceof Uint8Array) return x;
  if (x instanceof ArrayBuffer) return new Uint8Array(x);
  if (ArrayBuffer.isView(x)) return new Uint8Array(x.buffer, x.byteOffset, x.byteLength);
  throw new Error('不支持的字节类型');
}

// 编码一帧。caData/msg 传 Uint8Array（或可转视图）。返回 ArrayBuffer（可直接 ws.send）。
export function encodePDU(uiType, caData = null, msg = null) {
  const caDataBytes = toU8(caData);
  const msgBytes = toU8(msg);
  if (caDataBytes.length > CA_DATA_SIZE) {
    throw new Error(`caData 超长: ${caDataBytes.length} > ${CA_DATA_SIZE}`);
  }
  const total = HEADER_SIZE + msgBytes.length;
  const buf = new ArrayBuffer(total);
  const dv = new DataView(buf);
  dv.setUint32(0, total >>> 0, true);
  dv.setUint32(4, msgBytes.length >>> 0, true);
  dv.setUint32(8, uiType >>> 0, true);
  // ArrayBuffer 已全零，set 后未占用区自然保持 0（等价 mkPDU 的 memset）。
  new Uint8Array(buf, CA_DATA_OFFSET, CA_DATA_SIZE).set(caDataBytes);
  if (msgBytes.length) new Uint8Array(buf, CA_MSG_OFFSET, msgBytes.length).set(msgBytes);
  return buf;
}

// 解码一帧（入参须为完整帧的 ArrayBuffer）。caData/caMsg 是入参 buffer 上的视图，勿改。
export function decodePDU(arrayBuffer) {
  const dv = new DataView(arrayBuffer);
  const uiTotalLen = dv.getUint32(0, true);
  const uiMsgLen = dv.getUint32(4, true);
  const uiType = dv.getUint32(8, true);
  const u8 = new Uint8Array(arrayBuffer);
  const caData = u8.subarray(CA_DATA_OFFSET, CA_DATA_OFFSET + CA_DATA_SIZE);
  const caMsg = u8.subarray(CA_MSG_OFFSET, CA_MSG_OFFSET + uiMsgLen);
  return { uiTotalLen, uiMsgLen, uiType, caData, caMsg, buffer: arrayBuffer };
}
