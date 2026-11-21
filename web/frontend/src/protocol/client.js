// NetClient：网页端到服务端的传输层。管理 WebSocket、分帧、请求/响应匹配、
// 服务端 push 事件分发与心跳。对齐桌面端 Client 的连接职责（不含 UI）。
//
// 匹配策略：响应 uiType = 请求 uiType + 1。M1 流程（验证码/登录/注册/心跳）均由
// 用户或定时器串行发起，按“响应类型”登记 pending 足够；后续里程碑若出现并发同类
// 请求，再升级为带 request_id 的关联。
import { encodePDU, decodePDU } from './pdu.js';
import { Framer } from './framer.js';
import { T } from './msgtypes.js';
import { buildSessionCaData } from './session.js';

const DEFAULT_TIMEOUT_MS = 8000;
const HEARTBEAT_INTERVAL_MS = 30000; // 对齐桌面端 30s

export function defaultWsUrl() {
  const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
  return `${scheme}://${location.host}/ws`;
}

export class NetClient {
  constructor() {
    this.ws = null;
    this.framer = new Framer();
    this.pending = new Map();      // respType -> {resolve, reject, timer}
    this.pushHandlers = new Map(); // type -> Set<handler>
    this.openHandlers = new Set();
    this.closeHandlers = new Set();
    this.heartbeatTimer = null;
    this._hbWorker = null;   // 心跳 Worker（后台标签页防节流）
    this.lastPongMs = 0;
    this.connected = false;
    this.heartbeatCaData = null;
  }

  // 建立连接。resolve 于 WS open；reject 于连接失败。
  connect(url = defaultWsUrl()) {
    return new Promise((resolve, reject) => {
      let settled = false;
      const ws = new WebSocket(url);
      ws.binaryType = 'arraybuffer';
      this.ws = ws;

      ws.onopen = () => {
        settled = true;
        this.connected = true;
        this.framer.reset();
        this.lastPongMs = Date.now();
        this._startHeartbeat();
        for (const h of this.openHandlers) h();
        resolve();
      };
      ws.onerror = () => {
        if (!settled) { settled = true; reject(new Error('WebSocket 连接失败')); }
      };
      ws.onclose = (ev) => {
        this.connected = false;
        this._stopHeartbeat();
        this._rejectAllPending(new Error(`连接已关闭 (${ev.code}${ev.reason ? ' ' + ev.reason : ''})`));
        for (const h of this.closeHandlers) h(ev);
        if (!settled) { settled = true; reject(new Error(`连接关闭 (${ev.code})`)); }
      };
      ws.onmessage = (ev) => this._onMessage(ev.data);
    });
  }

  close(code = 1000, reason = 'client close') {
    this._stopHeartbeat();
    if (this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) {
      try { this.ws.close(code, reason); } catch { /* ignore */ }
    }
    this.connected = false;
  }

  // 设置心跳请求的 clientId 槽。未设置时保留旧客户端的空载荷兼容行为。
  setHeartbeatClientId(clientId) {
    this.heartbeatCaData = clientId ? buildSessionCaData(clientId) : null;
  }

  _onMessage(data) {
    const chunk = data instanceof ArrayBuffer
      ? new Uint8Array(data)
      : new Uint8Array(data.buffer || data, data.byteOffset || 0, data.byteLength || data.length);
    for (const frame of this.framer.push(chunk)) {
      // frame 是 Uint8Array；转成独立 ArrayBuffer 供 decodePDU 使用。
      const ab = frame.buffer.slice(frame.byteOffset, frame.byteOffset + frame.byteLength);
      const pdu = decodePDU(ab);
      this._dispatch(pdu);
    }
  }

  _dispatch(pdu) {
    if (pdu.uiType === T.HEARTBEAT_RESPOND) {
      this.lastPongMs = Date.now();
    }
    const p = this.pending.get(pdu.uiType);
    if (p) {
      this.pending.delete(pdu.uiType);
      clearTimeout(p.timer);
      p.resolve(pdu);
      return;
    }
    const set = this.pushHandlers.get(pdu.uiType);
    if (set) for (const h of set) { try { h(pdu); } catch (e) { console.error('push handler error', e); } }
  }

  // 发送一帧（不等待响应）。
  sendFrame(uiType, caData = null, msg = null) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      throw new Error('未连接');
    }
    if (uiType === T.HEARTBEAT_REQUEST && caData == null) caData = this.heartbeatCaData;
    this.ws.send(encodePDU(uiType, caData, msg));
  }

  // 发送请求并等待指定响应类型。返回解码后的响应 PDU。
  request(reqType, respType, caData = null, msg = null, timeoutMs = DEFAULT_TIMEOUT_MS) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      return Promise.reject(new Error('未连接'));
    }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(respType);
        reject(new Error(`请求超时 (type=${reqType})`));
      }, timeoutMs);
      this.pending.set(respType, { resolve, reject, timer });
      try {
        this.ws.send(encodePDU(reqType, caData, msg));
      } catch (e) {
        clearTimeout(timer);
        this.pending.delete(respType);
        reject(e);
      }
    });
  }

  // 注册服务端 push 事件处理器；返回取消函数。
  onPush(type, handler) {
    if (!this.pushHandlers.has(type)) this.pushHandlers.set(type, new Set());
    this.pushHandlers.get(type).add(handler);
    return () => this.pushHandlers.get(type)?.delete(handler);
  }
  onOpen(handler) { this.openHandlers.add(handler); return () => this.openHandlers.delete(handler); }
  onClose(handler) { this.closeHandlers.add(handler); return () => this.closeHandlers.delete(handler); }

  _rejectAllPending(err) {
    for (const [, p] of this.pending) { clearTimeout(p.timer); p.reject(err); }
    this.pending.clear();
  }

  _startHeartbeat() {
    this._stopHeartbeat();
    // 心跳定时器放 Web Worker：后台/隐藏标签页中主线程 setInterval 会被浏览器节流到
    // ≥1 分钟，超过服务端 90s 存活阈值会被踢线；Worker 定时器不受该节流影响。
    const tick = () => {
      if (this.ws && this.ws.readyState === WebSocket.OPEN) {
        try { this.sendFrame(T.HEARTBEAT_REQUEST, this.heartbeatCaData); } catch { /* ignore */ }
      }
    };
    try {
      const blob = new Blob(
        [`setInterval(function(){postMessage(1)},${HEARTBEAT_INTERVAL_MS})`],
        { type: 'text/javascript' });
      this._hbWorker = new Worker(URL.createObjectURL(blob));
      this._hbWorker.onmessage = tick;
    } catch {
      // Worker 不可用（如严格 CSP）时回退主线程定时器（前台标签页仍可靠）。
      this.heartbeatTimer = setInterval(tick, HEARTBEAT_INTERVAL_MS);
    }
  }
  _stopHeartbeat() {
    if (this.heartbeatTimer) { clearInterval(this.heartbeatTimer); this.heartbeatTimer = null; }
    if (this._hbWorker) { try { this._hbWorker.terminate(); } catch { /* ignore */ } this._hbWorker = null; }
  }
}
