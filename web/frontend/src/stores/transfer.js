// 传输 Store（引擎）：上传/下载任务队列、并发调度（上 3 / 下 3 独立预算）、
// 秒传预检单飞、停等分片、暂停/继续/取消（107）、下载聚合与保存。
// 协议帧构造/解析全部复用 protocol/commands/drive.js（e2e 与 GUI 共享同一字节实现）。
//
// 传输族响应（30/32/41/42/43/98/108）按 transfer_id 路由——不能用 NetClient 的
// 请求-响应匹配（同类响应可并发多任务）。断线时全部在途任务收敛为 Failed（对齐桌面端
// G7），排队/暂停任务保留可"继续"。
//
// 下载统一为"增量 Blob 聚合 + a[download] 浏览器原生保存"（M3 验收缺陷终案）：
// 弃用 File System Access API——真实 Chrome 下 showSaveFilePicker 会立即把正式文件
// 创建/截断为 0 字节、数据写同目录 .crswap、close 才合并，导致"暂停后重选句柄截断
// 前缀""取消只删暂存、遗留 0 字节正式文件"两类无法根治的残留。Blob 方案不预先创建
// 任何浏览器文件：收完并校验字节数后才触发一次下载，暂停/取消/失败均零文件残留；
// 会话内续传点 = 内存已聚合字节。大数据由 Chrome Blob 自动磁盘后备，不占等量内存。
//
// 下载运行体采用 epoch 失效令牌：暂停/取消使当前运行体作废，其在 40→41 受理窗口、
// 43 等待等任何 await 点醒来后一律静默退出，不出现僵尸运行体误报失败/翻改终态。
import { defineStore } from 'pinia';
import { getNet } from '../protocol/net.js';
import { T } from '../protocol/msgtypes.js';
import * as drive from '../protocol/commands/drive.js';
import Sha256Worker from '../workers/sha256.worker.js?worker';

// ===== 模块级引擎状态 =====
let idCounter = 0n;             // 会话级 transfer_id，从 1 单调递增（0 为服务端旧单流保留值）
let keySeq = 0;                 // 任务行键
const tidToKey = new Map();     // tid 字符串 -> 任务 key（30/32/41/42/43 路由）
const waiters = new Map();      // 等待键 -> {resolve, timer|null}
const dlState = new Map();      // 下载任务 key -> {blobs, blobBytes, parts, partsBytes, epoch}
let fcCurrentReqId = '';        // 秒传单飞：在途 97 的请求标识
let fcQueue = Promise.resolve(); // 秒传单飞链（串行化 97）
let uploadWorker = null;        // 惰性单例 Worker
let engineBound = null;         // 已绑定 push 路由的 NetClient

// 内存分片达到 64MiB 就滚存为 Blob 段，防止 parts 数组无上限堆积原始 Uint8Array。
const PART_FLUSH_BYTES = 64 * 1024 * 1024;

const allocId = () => (idCounter += 1n);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const newDlState = () => ({ blobs: [], blobBytes: 0, parts: [], partsBytes: 0, epoch: 0 });
const dlTotal = (ds) => ds.blobBytes + ds.partsBytes;
const dlDrop = (ds) => { ds.blobs = []; ds.blobBytes = 0; ds.parts = []; ds.partsBytes = 0; };

// timeoutMs <= 0 表示不设超时（下载完成事件可 long-running，断线由 onClose 收敛）。
function waitFor(wkey, timeoutMs, desc) {
  return new Promise((resolve, reject) => {
    let timer = null;
    if (timeoutMs > 0) {
      timer = setTimeout(() => { waiters.delete(wkey); reject(new Error(`${desc}等待超时`)); }, timeoutMs);
    }
    waiters.set(wkey, {
      resolve: (v) => { if (timer) clearTimeout(timer); waiters.delete(wkey); resolve(v); },
      timer,
    });
  });
}
function deliver(wkey, value) {
  const w = waiters.get(wkey);
  if (w) w.resolve(value);
}
// 以 {__error} 收敛等待者（调用方检查后抛出），前缀匹配。
function rejectWaiters(prefix, err) {
  for (const [k, w] of [...waiters.entries()]) {
    if (k.startsWith(prefix)) {
      if (w.timer) clearTimeout(w.timer);
      waiters.delete(k);
      w.resolve({ __error: err });
    }
  }
}
// 以 {__stale} 唤醒指定下载运行体的等待点（暂停/取消用；运行体静默退出）。
function staleWake(tid) {
  deliver(`d41:${tid}`, { __stale: true });
  deliver(`dlfin:${tid}`, { __stale: true });
}

// Worker 增量 SHA-256；onProgress(doneBytes)。
function hashFileSha256(file, onProgress) {
  return new Promise((resolve, reject) => {
    if (!uploadWorker) uploadWorker = new Sha256Worker();
    const id = `h${Date.now()}_${Math.random().toString(36).slice(2)}`;
    const onMsg = (e) => {
      const d = e.data;
      if (!d || d.id !== id) return;
      if (d.done) { uploadWorker.removeEventListener('message', onMsg); resolve(d.shaHex); }
      else if (d.error) { uploadWorker.removeEventListener('message', onMsg); reject(new Error(d.error)); }
      else if (onProgress) onProgress(d.progress);
    };
    uploadWorker.addEventListener('message', onMsg);
    uploadWorker.postMessage({ id, file });
  });
}

// Blob → a[download] 触发浏览器保存（保存到浏览器默认下载目录，重名由浏览器加 (n)；
// 浏览器开启"每次下载前询问保存位置"时逐次询问）。
function saveBlobAs(blob, name) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
}

export const useTransferStore = defineStore('transfer', {
  state: () => ({
    tasks: [], // [{key,kind,name,state,doneBytes,totalBytes,failReason,finalName,...}]
    initialized: false,
  }),
  getters: {
    activeCount: (s) => s.tasks.filter((t) => ['running', 'hashing', 'saving'].includes(t.state)).length,
  },
  actions: {
    // ===== 引擎接线（MainView 挂载时调用；同一 NetClient 只绑一次）=====
    init() {
      const net = getNet();
      if (engineBound === net) { this.initialized = true; return; }
      engineBound = net;

      net.onPush(T.UPLOADFILE_FASTCHECK_RESPOND, (pdu) => {
        const fc = drive.parseFastcheckResponse(pdu);
        const key = fc.reqIdEcho || fcCurrentReqId; // 按回带标识归属；迟到 98 标识不符则无人认领自然丢弃
        if (key) deliver(`fc:${key}`, fc);
      });
      net.onPush(T.UPLOADFILE_INIT_RESPOND, (pdu) => {
        const r = drive.parseUploadInitResponse(pdu);
        deliver(`u30:${r.tid}`, r);
      });
      net.onPush(T.UPLOADFILE_DATA_RESPOND, (pdu) => {
        const r = drive.parseUploadAck(pdu);
        deliver(`u32:${r.tid}`, r);
      });
      net.onPush(T.DOWNLOADFILE_RESPOND, (pdu) => {
        const r = drive.parseDownloadResponse(pdu);
        deliver(`d41:${r.tid}`, r);
      });
      net.onPush(T.DOWNLOADFILE_DATA, (pdu) => this._onDownloadData(pdu));
      net.onPush(T.DOWNLOADFILE_FINISH, (pdu) => {
        const { success, tid } = drive.parseDownloadFinish(pdu);
        deliver(`dlfin:${tid}`, { success });
      });
      net.onPush(T.UPLOADFILE_ABORT_RESPOND, () => { /* 108 仅确认，暂停/取消已本地落定 */ });

      // 断线收敛（桌面端 G7 同款）：在途判失败并摘 id；排队/暂停保留。
      // 下载内存缓冲是"已收到"的字节，断线不作废——重连点"继续"从 doneBytes 续传。
      net.onClose(() => {
        for (const t of this.tasks) {
          if (['running', 'hashing', 'saving'].includes(t.state)) {
            if (t.tid) tidToKey.delete(t.tid.toString());
            if (t.kind === 'download') {
              const ds = dlState.get(t.key);
              if (ds) ds.epoch++; // 作废在途运行体（缓冲保留供续传）
            }
            this._patch(t.key, { state: 'failed', failReason: '与服务端的连接已断开' });
          }
        }
        for (const p of ['fc:', 'u30:', 'u32:', 'd41:', 'dlfin:']) rejectWaiters(p, new Error('连接断开'));
      });

      this.initialized = true;
    },

    _task(key) { return this.tasks.find((t) => t.key === key); },
    _patch(key, patch) {
      const i = this.tasks.findIndex((t) => t.key === key);
      if (i >= 0) this.tasks[i] = { ...this.tasks[i], ...patch };
    },
    // 用户已落定的终态（暂停/取消/完成）不被迟到异常改写。
    _failUnlessSettled(key, e) {
      const cur = this._task(key);
      if (!cur) return; // 已被移除（取消）
      if (['paused', 'cancelled', 'done'].includes(cur.state)) return;
      this._patch(key, { state: 'failed', failReason: (e && e.message) || String(e) });
    },

    // ===== 入队 =====
    enqueueUpload(file, dirAbsPath) {
      const name = file.name;
      if (!drive.isValidFileName(name)) {
        this.tasks.push({
          key: ++keySeq, kind: 'upload', name, state: 'failed', doneBytes: 0, totalBytes: file.size,
          failReason: '文件名不合法（≤31 字节，不含 <>:"/\\|?* 与控制字符）', finalName: '', speed: 0,
        });
        return null;
      }
      this.tasks.push({
        key: ++keySeq, kind: 'upload', name, dirPath: dirAbsPath, file,
        state: 'queued', doneBytes: 0, totalBytes: file.size, failReason: '', finalName: '',
        shaHex: '', tid: null, control: null, skipFastcheck: false, speed: 0, hashProgress: 0,
      });
      this._pump();
      return keySeq;
    },
    enqueueDownload(serverAbsPath, name) {
      this.tasks.push({
        key: ++keySeq, kind: 'download', name, serverPath: serverAbsPath,
        state: 'queued', doneBytes: 0, totalBytes: 0, failReason: '', finalName: '',
        tid: null, control: null, speed: 0,
      });
      this._pump();
      return keySeq;
    },

    // ===== 调度 =====
    _engine() {
      if (!this._eng) this._eng = { activeUploads: 0, activeDownloads: 0 };
      return this._eng;
    },
    _pump() {
      const eng = this._engine();
      if (!getNet().connected) return; // 断线期间不启动新任务
      while (eng.activeUploads < drive.MAX_PARALLEL_UPLOADS) {
        const t = this.tasks.find((x) => x.kind === 'upload' && x.state === 'queued');
        if (!t) break;
        eng.activeUploads++;
        this._runUpload(t.key);
      }
      while (eng.activeDownloads < drive.MAX_PARALLEL_DOWNLOADS) {
        const t = this.tasks.find((x) => x.kind === 'download' && x.state === 'queued');
        if (!t) break;
        eng.activeDownloads++;
        this._runDownload(t.key);
      }
    },
    _release(kind) {
      const eng = this._engine();
      if (kind === 'upload') eng.activeUploads = Math.max(0, eng.activeUploads - 1);
      else eng.activeDownloads = Math.max(0, eng.activeDownloads - 1);
      this._pump();
    },

    // ===== 上传状态机：SHA → 97 单飞 → 29/30 → 31/32 停等 →（107 暂停/取消）=====
    async _runUpload(key) {
      const net = getNet();
      let t = this._task(key);
      if (!t) { this._release('upload'); return; }
      this._patch(key, { state: 'hashing', failReason: '' });
      try {
        t = this._task(key);
        if (!t.shaHex) {
          const sha = await hashFileSha256(t.file, (p) => this._patch(key, { hashProgress: p }));
          this._patch(key, { shaHex: sha });
        }
        t = this._task(key);
        if (t.control === 'cancel') { this._patch(key, { state: 'cancelled', control: null }); return; }
        if (t.control === 'pause') { this._patch(key, { state: 'paused', control: null }); return; }
        this._patch(key, { state: 'running' });

        // 97 秒传预检（单飞）；"继续"的任务跳过（staging 已在服务端，直接 29）
        t = this._task(key);
        if (!t.skipFastcheck) {
          const fc = await this._fastcheckSingleFlight(t);
          if (fc.__error) throw fc.__error;
          if (fc.version !== drive.FASTCHECK_VERSION) throw new Error('服务端秒传协议版本不匹配');
          if (!fc.ret) throw new Error('服务端拒绝秒传预检查');
          if (fc.hit) {
            this._patch(key, { state: 'done', doneBytes: t.totalBytes, finalName: fc.finalName || t.name, speed: 0 });
            return;
          }
        }
        t = this._task(key);
        if (t.control === 'cancel') { this._patch(key, { state: 'cancelled', control: null }); return; }
        if (t.control === 'pause') { this._patch(key, { state: 'paused', control: null }); return; }

        // 29/30 初始化（ret=false=并发上限/id 冲突 → 退避重试）
        let init = null;
        for (let attempt = 0; attempt < 5 && !init; attempt++) {
          const tid = allocId();
          this._patch(key, { tid: tid.toString() });
          tidToKey.set(tid.toString(), key);
          const f = drive.buildUploadInitFrame({ name: t.name, fileSize: BigInt(t.file.size), tid, dirAbsPath: t.dirPath });
          const w = waitFor(`u30:${tid}`, 15000, '上传初始化');
          net.sendFrame(f.type, f.caData, f.msg);
          const r = await w;
          if (r.__error) throw r.__error;
          if (r.ret) { init = r; }
          else { tidToKey.delete(tid.toString()); await sleep(300 + attempt * 300); }
          t = this._task(key);
          if (t.control) break; // 暂停/取消：跳出交给块边界处理
        }
        t = this._task(key);
        if (t.control === 'cancel') { this._patch(key, { state: 'cancelled', control: null }); return; }
        if (t.control === 'pause') { this._patch(key, { state: 'paused', control: null }); return; }
        if (!init) throw new Error('上传初始化屡被拒绝（服务端并发上限），请稍后点"继续"');

        // 31/32 停等分片
        let offset = Number(init.resumeOffset);
        if (BigInt(offset) >= BigInt(t.file.size)) {
          this._patch(key, { state: 'done', doneBytes: t.file.size, speed: 0 }); // 0 字节或断点已在尾部
          return;
        }
        const chunkSize = drive.chunkSizeFor(t.file.size);
        while (offset < t.file.size) {
          t = this._task(key);
          if (t.control === 'cancel' || t.control === 'pause') {
            const af = drive.buildAbortFrame(BigInt(t.tid));
            net.sendFrame(af.type, af.caData, af.msg); // 107：staging 截齐 confirmed、会话 expired（可续）
            this._patch(key, { state: t.control === 'cancel' ? 'cancelled' : 'paused', control: null, speed: 0 });
            return;
          }
          const prev = offset;
          const end = Math.min(offset + chunkSize, t.file.size);
          const buf = new Uint8Array(await t.file.slice(offset, end).arrayBuffer());
          const ackP = waitFor(`u32:${BigInt(t.tid)}`, 60000, '分片确认');
          const cf = drive.buildUploadChunkFrame({ offset: BigInt(offset), tid: BigInt(t.tid), chunk: buf });
          net.sendFrame(cf.type, cf.caData, cf.msg);
          const ack = await ackP;
          if (ack.__error) throw ack.__error;
          if (!ack.ret) throw new Error('分片被服务端拒绝（偏移不符）');
          const next = Number(ack.nextOffset);
          if (next < prev) throw new Error('确认偏移回退，终止上传');
          offset = next;
          this._patch(key, { doneBytes: offset });
          if (ack.complete) { this._patch(key, { state: 'done', doneBytes: t.file.size, speed: 0 }); return; }
        }
        throw new Error('文件已读完但未收到完成确认');
      } catch (e) {
        this._failUnlessSettled(key, e);
      } finally {
        const cur = this._task(key);
        if (cur && cur.tid) tidToKey.delete(cur.tid.toString());
        this._release('upload');
      }
    },

    // 97 单飞：串行链 + reqId 归属。
    _fastcheckSingleFlight(t) {
      const run = async () => {
        const net = getNet();
        const reqId = drive.newReqId();
        fcCurrentReqId = reqId;
        const f = drive.buildFastcheckFrame({
          shaHex: t.shaHex, name: t.name, dirAbsPath: t.dirPath, fileSize: BigInt(t.file.size), reqId,
        });
        const w = waitFor(`fc:${reqId}`, 20000, '秒传预检');
        net.sendFrame(f.type, f.caData, f.msg);
        const r = await w;
        if (fcCurrentReqId === reqId) fcCurrentReqId = '';
        return r;
      };
      const result = fcQueue.then(run, run);
      fcQueue = result.then(() => {}, () => {});
      return result;
    },

    // ===== 下载状态机：40/41 → 42 流式聚合 → 43 完成 → 校验字节数 → a[download] =====
    async _runDownload(key) {
      const net = getNet();
      let t = this._task(key);
      if (!t) { this._release('download'); return; }
      let ds = dlState.get(key);
      if (!ds) { ds = newDlState(); dlState.set(key, ds); }
      const myEpoch = ++ds.epoch;      // 本次运行体令牌；暂停/取消通过再自增使其失效
      const stale = () => ds.epoch !== myEpoch;
      this._patch(key, { state: 'running', failReason: '', speed: 0 });
      try {
        const tid = allocId();
        this._patch(key, { tid: tid.toString() });
        tidToKey.set(tid.toString(), key);
        t = this._task(key);

        const requested = dlTotal(ds); // 会话内续传：声明已聚合字节
        if (requested !== t.doneBytes) this._patch(key, { doneBytes: requested });
        const f = drive.buildDownloadFrame({ name: t.name, resumeOffset: BigInt(requested), tid, serverAbsPath: t.serverPath });
        const w41 = waitFor(`d41:${tid}`, 15000, '下载受理');
        net.sendFrame(f.type, f.caData, f.msg);
        const r41 = await w41;
        if (stale()) return;           // 受理窗口内被暂停/取消：静默退出（finally 归还名额）
        if (r41.__stale) return;
        if (r41.__error) throw r41.__error;
        if (!r41.ret) {
          if (requested > 0) {
            dlDrop(ds); this._patch(key, { doneBytes: 0 });
            throw new Error('服务端不接受续传起点，缓冲已清空，点"继续"将整文件重下');
          }
          throw new Error('服务端拒绝下载请求');
        }
        const size = Number(r41.fileSize);
        if (requested > 0 && Number(r41.accepted) !== requested) {
          dlDrop(ds); this._patch(key, { doneBytes: 0 });
          throw new Error(`服务端采纳的续传起点与请求不符（请求 ${requested}，采纳 ${r41.accepted}），缓冲已清空，点"继续"将整文件重下`);
        }
        if (requested > 0 && t.totalBytes > 0 && size !== t.totalBytes) {
          dlDrop(ds);
          this._patch(key, { doneBytes: 0, totalBytes: size });
          throw new Error('服务端文件已变化，无法续传（已清空缓冲，请点"继续"重新下载）');
        }
        if (requested > size) {        // 理论上不可能（缓冲≤上次确认的 size），防御服务端变化
          dlDrop(ds); this._patch(key, { doneBytes: 0 });
          throw new Error('缓冲字节超过服务端文件大小，缓冲已清空，点"继续"将整文件重下');
        }
        this._patch(key, { totalBytes: size });

        // 42 由 push 路由聚合；43 完成事件无固定超时（大文件 long-running，断线由 onClose 收敛）。
        const fin = await waitFor(`dlfin:${tid}`, 0, '下载');
        if (stale()) return;
        if (fin.__stale) return;
        if (fin.__error) throw fin.__error;
        if (!fin.success) throw new Error('服务端报告下载失败');

        // 收尾前再验令牌与字节数：绝不把不完整内容交给浏览器保存。
        this._patch(key, { state: 'saving', speed: 0 });
        const total = dlTotal(ds);
        if (total !== size) {
          throw new Error(`下载字节数不符（已聚合 ${total}，应为 ${size}），未保存任何文件；点"继续"从断点续传`);
        }
        if (stale()) return;
        saveBlobAs(new Blob(ds.blobs.concat(ds.parts), { type: 'application/octet-stream' }), t.name || 'download.bin');
        dlDrop(ds);
        if (stale()) return;
        ds.epoch++; // 本运行体已收尾：任何迟到事件不得再次收尾
        this._patch(key, { state: 'done', doneBytes: size, finalName: t.name });
      } catch (e) {
        if (!stale()) this._failUnlessSettled(key, e);
      } finally {
        const cur = this._task(key);
        if (cur && cur.tid) tidToKey.delete(cur.tid.toString());
        this._release('download');
      }
    },

    // 42：追加到内存分片，满 64MiB 滚存为 Blob 段；未知 id/非 Running 落空。
    _onDownloadData(pdu) {
      const { tid, chunk } = drive.parseDownloadData(pdu);
      const key = tidToKey.get(tid.toString());
      if (key == null) return;
      const t = this._task(key);
      if (!t || t.state !== 'running') return;
      const ds = dlState.get(key);
      if (!ds) return;
      const bytes = new Uint8Array(chunk);
      ds.parts.push(bytes);
      ds.partsBytes += bytes.length;
      if (ds.partsBytes >= PART_FLUSH_BYTES) {
        ds.blobs.push(new Blob(ds.parts, { type: 'application/octet-stream' }));
        ds.blobBytes += ds.partsBytes;
        ds.parts = [];
        ds.partsBytes = 0;
      }
      this._patch(key, { doneBytes: dlTotal(ds) });
    },

    // ===== 用户操作 =====
    // 暂停：下载立即生效（107 停发、作废运行体、保留内存缓冲供续传）；
    // 上传在当前分片边界生效（≤1 块延迟，由运行体自查 control 落定）。
    // saving（收尾保存中）不可暂停——已收到 43，暂停无意义且引入竞态；可取消。
    pause(key) {
      const t = this._task(key);
      if (!t) return;
      if (t.state === 'queued') { this._patch(key, { state: 'paused' }); return; }
      if (t.state !== 'running' && t.state !== 'hashing') return;
      if (t.kind === 'download') {
        if (t.tid) {
          const ds = dlState.get(key);
          if (ds) ds.epoch++; // 作废在途运行体（其醒来后静默退出并归还名额）
          tidToKey.delete(t.tid.toString());
          try {
            const af = drive.buildAbortFrame(BigInt(t.tid));
            getNet().sendFrame(af.type, af.caData, af.msg);
          } catch { /* ignore */ }
          staleWake(BigInt(t.tid)); // 唤醒 d41/dlfin 等待点
        }
        this._patch(key, { state: 'paused', control: null, speed: 0 });
        return;
      }
      this._patch(key, { control: 'pause' }); // 上传：停等循环在块边界收尾
    },
    // 取消：下载立即（107 + 作废运行体 + 丢弃缓冲 + 移除行；浏览器侧零文件残留）；
    // 上传在块边界。
    cancel(key) {
      const t = this._task(key);
      if (!t) return;
      if (t.state === 'queued') { this._remove(key); return; }
      if (t.state === 'running' || t.state === 'hashing' || t.state === 'saving') {
        if (t.kind === 'download') {
          const ds = dlState.get(key);
          if (ds) ds.epoch++;
          if (t.tid) {
            tidToKey.delete(t.tid.toString());
            try {
              const af = drive.buildAbortFrame(BigInt(t.tid));
              getNet().sendFrame(af.type, af.caData, af.msg);
            } catch { /* ignore */ }
            staleWake(BigInt(t.tid));
          }
          this._remove(key); // finally 中 _task 为 undefined → 名额由运行体退出时归还
          return;
        }
        this._patch(key, { control: 'cancel' });
        return; // 上传由停等循环落定 cancelled
      }
      this._remove(key);
    },
    // 继续/重试：上传换新 tid 直接 29（服务端按 staging 派生续传点）；
    // 下载以内存已聚合字节声明起点（缓冲保留时断点续传，被清空时整文件重下）。
    resume(key) {
      const t = this._task(key);
      if (!t || (t.state !== 'paused' && t.state !== 'failed')) return;
      if (t.kind === 'download') {
        const ds = dlState.get(key);
        const wantDrops = /文件已变化|起点|重下|清空/.test(t.failReason || '');
        if (ds && (!ds.blobs.length && !ds.parts.length)) this._patch(key, { doneBytes: 0 });
        else if (ds && wantDrops) { dlDrop(ds); this._patch(key, { doneBytes: 0 }); }
        else if (ds) this._patch(key, { doneBytes: dlTotal(ds) });
      }
      this._patch(key, { state: 'queued', control: null, failReason: '', skipFastcheck: t.kind === 'upload' });
      this._pump();
    },
    _remove(key) {
      const i = this.tasks.findIndex((t) => t.key === key);
      if (i >= 0) {
        const t = this.tasks[i];
        if (t.tid) tidToKey.delete(t.tid.toString());
        this.tasks.splice(i, 1);
      }
      const ds = dlState.get(key);
      if (ds) {
        ds.epoch++;
        dlDrop(ds);
        dlState.delete(key);
      }
    },
    clearFinished() {
      for (const t of this.tasks) {
        if (['done', 'cancelled', 'failed'].includes(t.state)) dlState.delete(t.key);
      }
      this.tasks = this.tasks.filter((t) => !['done', 'cancelled', 'failed'].includes(t.state));
    },
    reset() {
      for (const [, w] of waiters) { if (w.timer) clearTimeout(w.timer); }
      waiters.clear();
      for (const [, ds] of dlState) { ds.epoch++; dlDrop(ds); }
      dlState.clear();
      tidToKey.clear();
      fcCurrentReqId = '';
      this.tasks = [];
      this._eng = { activeUploads: 0, activeDownloads: 0 };
    },
  },
});
