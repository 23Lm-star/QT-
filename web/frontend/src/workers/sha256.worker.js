// Web Worker：大文件增量 SHA-256（hash-wasm），1MiB 分块——与桌面端 QtConcurrent 分块一致。
// 消息协议：{id, file} → 进度 {id, progress} → 终态 {id, done:true, shaHex} 或 {id, error}。
import { createSHA256 } from 'hash-wasm';

const CHUNK = 1024 * 1024;

self.onmessage = async (e) => {
  const { id, file } = e.data;
  try {
    const hasher = await createSHA256();
    hasher.init();
    for (let off = 0; off < file.size; off += CHUNK) {
      const buf = await file.slice(off, Math.min(off + CHUNK, file.size)).arrayBuffer();
      hasher.update(new Uint8Array(buf));
      self.postMessage({ id, progress: Math.min(off + CHUNK, file.size) });
    }
    self.postMessage({ id, done: true, shaHex: hasher.digest('hex') });
  } catch (err) {
    self.postMessage({ id, error: String((err && err.message) || err) });
  }
};
