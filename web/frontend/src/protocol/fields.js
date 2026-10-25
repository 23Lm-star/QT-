// 字段级读写工具：定长字符串、u32/i32/u64（小端），语义对齐 Client/client.cpp 的
// copyFixedField / copyExactField 与 msghandler.cpp 的 memcpy 解析。
const enc = new TextEncoder();
const dec = new TextDecoder('utf-8');

// copyFixedField 语义：写入定长区并保证 NUL 结尾，最多 size-1 字节（其余保持 0）。
export function writeFixedStringNul(u8, offset, size, str) {
  const bytes = enc.encode(str);
  const n = Math.min(bytes.length, size - 1);
  u8.set(bytes.subarray(0, n), offset);
}

// copyExactField 语义：恰好填满 size 字节，不留 NUL（用于 32 字节 MD5 hex 等）。
export function writeExactBytes(u8, offset, size, bytes) {
  const n = Math.min(bytes.length, size);
  u8.set(bytes.subarray(0, n), offset);
}

// 直接写字节（不补齐、不截断到定长区），用于 caMsg 拼接。
export function writeBytes(u8, offset, bytes) {
  u8.set(bytes, offset);
}

// 读取到首个 NUL 为止并按 UTF-8 解码（caData 定长字符串、caMsg 文本）。
export function readFixedString(u8, offset = 0, size = u8.length - offset) {
  const stop = Math.min(offset + size, u8.length);
  let end = offset;
  while (end < stop && u8[end] !== 0) end++;
  return dec.decode(u8.subarray(offset, end));
}

// 读取定长字节区并按 UTF-8 解码（不截 NUL，用于可能无 NUL 的定长名，如 FileInfo.caName）。
export function readBytesString(u8, offset, size) {
  return dec.decode(u8.subarray(offset, offset + size));
}

function view(u8) {
  return new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
}

export function readU32(u8, offset) { return view(u8).getUint32(offset, true); }
export function readI32(u8, offset) { return view(u8).getInt32(offset, true); }
export function readU16(u8, offset) { return view(u8).getUint16(offset, true); }
// u64 一律 BigInt，禁止 Number（transfer_id/file_size 会超 2^53）。
export function readU64(u8, offset) { return view(u8).getBigUint64(offset, true); }

export function u32ToBytes(v) {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, v >>> 0, true);
  return b;
}
export function u16ToBytes(v) {
  const b = new Uint8Array(2);
  new DataView(b.buffer).setUint16(0, v & 0xffff, true);
  return b;
}
export function u64ToBytes(v) {
  const b = new Uint8Array(8);
  new DataView(b.buffer).setBigUint64(0, BigInt(v), true);
  return b;
}

export { enc as textEncoder, dec as textDecoder };

// 拼接多个字节片段为一个 Uint8Array（构造 caMsg 用）。
export function concatBytes(...parts) {
  const list = parts.map(toU8Local);
  const total = list.reduce((s, b) => s + b.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const b of list) { out.set(b, off); off += b.length; }
  return out;
}
function toU8Local(x) {
  if (x == null) return new Uint8Array(0);
  if (x instanceof Uint8Array) return x;
  if (typeof x === 'string') return enc.encode(x);
  if (x instanceof ArrayBuffer) return new Uint8Array(x);
  if (ArrayBuffer.isView(x)) return new Uint8Array(x.buffer, x.byteOffset, x.byteLength);
  throw new Error('concatBytes: 不支持的类型');
}

// UTF-8 字节长度（前端输入校验：用户名 ≤31 字节，须按字节而非字符计）。
export function utf8Length(str) { return enc.encode(str).length; }
