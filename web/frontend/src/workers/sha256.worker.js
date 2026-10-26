// Web Worker：大文件增量 SHA-256（hash-wasm）。主线程按 1MiB 读取 File 后分片送入，
// 避免把整个 File 对象 postMessage 给 Worker（部分移动端 WebView 不支持 File 克隆）。
// 消息协议：{id,type:'init'} → ready；{id,type:'chunk',buf} → progress；{id,type:'end'} → done/error。
import { createSHA256 } from 'hash-wasm';

const hashers = new Map();

self.onmessage = async (e) => {
  const { id, type, buf } = e.data || {};
  try {
    if (type === 'init') {
      const hasher = await createSHA256();
      hasher.init();
      hashers.set(id, { hasher, done: 0 });
      self.postMessage({ id, ready: true });
    } else if (type === 'chunk') {
      const st = hashers.get(id);
      if (!st) throw new Error('未知哈希任务');
      st.hasher.update(new Uint8Array(buf));
      st.done += buf.byteLength;
      self.postMessage({ id, progress: st.done });
    } else if (type === 'end') {
      const st = hashers.get(id);
      if (!st) throw new Error('未知哈希任务');
      const shaHex = st.hasher.digest('hex');
      hashers.delete(id);
      self.postMessage({ id, done: true, shaHex });
    }
  } catch (err) {
    hashers.delete(id);
    self.postMessage({ id, error: String((err && err.message) || err) });
  }
};
