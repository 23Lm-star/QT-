// 分帧器：把网关转发来的 TCP 流（边界任意）按 uiTotalLen 切成完整 PDU。
// 逻辑对齐 Client/client.cpp::recvmsg 的 m_pdulist 循环：
//   while 缓冲 >= 76 且 缓冲 >= 头部声明的 uiTotalLen，切出一帧派发。
import { HEADER_SIZE } from './pdu.js';

// 单帧字节上限：合法最大帧为上传数据块（远小于此值），坏帧在此直接丢弃
export const MAX_FRAME_BYTES = 16 * 1024 * 1024;

export class Framer {
  constructor() {
    this.buf = new Uint8Array(0);
  }

  // 追加一段字节，返回本次可切出的完整帧数组（每帧是独立 Uint8Array）。
  push(chunk) {
    const merged = new Uint8Array(this.buf.length + chunk.length);
    merged.set(this.buf, 0);
    merged.set(chunk, this.buf.length);
    this.buf = merged;

    const frames = [];
    while (this.buf.length >= HEADER_SIZE) {
      const dv = new DataView(this.buf.buffer, this.buf.byteOffset, this.buf.byteLength);
      const total = dv.getUint32(0, true);
      // REQ-AUDIT-FULL-001：单帧上限钳制——头部声明超大的坏帧直接判流损坏，
      // 避免半包场景下缓冲被恶意/损坏的 total 无限撑大。
      if (total > MAX_FRAME_BYTES) {
        this.buf = new Uint8Array(0);
        break;
      }
      if (total < HEADER_SIZE) {
        // 头部声明非法，协议流已损坏；丢弃缓冲避免死循环，交由上层判定断线。
        this.buf = new Uint8Array(0);
        break;
      }
      if (this.buf.length >= total) {
        frames.push(this.buf.slice(0, total));
        this.buf = this.buf.slice(total);
      } else {
        break; // 半包，等下一段。
      }
    }
    return frames;
  }

  reset() {
    this.buf = new Uint8Array(0);
  }
}
