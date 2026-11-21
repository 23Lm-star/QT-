// 认证/连接 Store（Pinia）。NetClient 保持在模块作用域（非响应式），
// 避免 Vue 深度代理 WebSocket/定时器；Store 只保存可序列化的界面状态。
import { defineStore } from 'pinia';
import { defaultWsUrl } from '../protocol/client.js';
import { getNet } from '../protocol/net.js';
import * as account from '../protocol/commands/account.js';
import { T } from '../protocol/msgtypes.js';
import { parseInvalidatedEvent } from '../protocol/session.js';

let net = null; // 指向模块级共享单例（protocol/net.js），auth 负责挂接连接状态回调

// 生成 32 位 hex 客户端 ID，等价 QUuid::createUuid().toString(Id128)。
function genClientId() {
  const b = new Uint8Array(16);
  crypto.getRandomValues(b);
  return Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
}
function loadClientId() {
  let id = '';
  try { id = localStorage.getItem('cloudpan.clientId') || ''; } catch { id = ''; }
  if (!/^[0-9a-f]{32}$/.test(id)) {
    id = genClientId();
    try { localStorage.setItem('cloudpan.clientId', id); } catch { /* ignore */ }
  }
  return id;
}
function loadSessionValue(key) {
  try { return localStorage.getItem(key) || ''; } catch { return ''; }
}
function saveSessionValue(key, value) {
  try {
    if (value) localStorage.setItem(key, value);
    else localStorage.removeItem(key);
  } catch { /* 浏览器存储不可用时仍保持内存会话 */ }
}

export const useAuthStore = defineStore('auth', {
  state: () => ({
    clientId: '',
    resumeToken: '',
    tokenUser: '',
    sessionActive: false,
    resumePending: false,
    connectState: 'idle', // idle | connecting | open | closed
    lastClose: null,     // {code,reason,text,at} 最近一次断线原因，供主界面显示
    loggedIn: false,
    loginName: '',
    captchaImage: '',
    captchaCooldown: 0,
    message: '',
    messageType: 'info', // info | error | success
  }),
  getters: {
    isConnected: (s) => s.connectState === 'open',
  },
  actions: {
    _ensureNet() {
      if (net) return net;
      net = getNet((n) => {
        n.onOpen(() => { this.connectState = 'open'; this.lastClose = null; });
        n.onClose((ev) => {
          this.connectState = 'closed';
          this.sessionActive = false;
          // 有恢复凭据时保留登录态和主界面，等待用户手动重连后恢复。
          if (!this.resumeToken) {
            this.loggedIn = false;
            this.loginName = '';
          }
          // 区分断线原因，便于公网/移动端排障：网关自定义码、上游关闭（被踢/回收/重启）、
          // 异常码（隧道或网络中断）分别给出可判读的文案。
          const code = ev && typeof ev.code === 'number' ? ev.code : 0;
          const rs = (ev && ev.reason) || '';
          let text = '与服务端的连接已断开';
          if (code === 4001) text = '无法连接到服务端（上游 TCP 失败）';
          else if (code === 4003) text = '网关连接数已满';
          else if (rs === 'upstream closed') text = '服务端关闭了连接（多为账号在其他端登录被踢，或服务端回收/重启）';
          else if (code === 1006) text = '网络或隧道中断（连接异常关闭 1006）';
          else if (code === 1011) text = '网关/隧道转发异常（1011）';
          else if (code) text = `连接已断开（code=${code}${rs ? ' ' + rs : ''}）`;
          this.lastClose = { code, reason: rs, text, at: Date.now() };
          this._setMsg(text, 'error');
        });
        n.onPush(T.SESSION_INVALIDATED_EVENT, (pdu) => this._handleInvalidated(pdu));
      });
      if (this.clientId) net.setHeartbeatClientId(this.clientId);
      return net;
    },
    init() {
      if (!this.clientId) this.clientId = loadClientId();
      if (!this.resumeToken) this.resumeToken = loadSessionValue('cloudpan.resumeToken');
      if (!this.tokenUser) this.tokenUser = loadSessionValue('cloudpan.resumeUser');
      this._ensureNet();
      net.setHeartbeatClientId(this.clientId);
    },
    _setMsg(text, type = 'info') { this.message = text; this.messageType = type; },

    async connect() {
      this.init();
      if (net && net.connected) {
        if (this.resumeToken && !this.sessionActive) await this.tryResumeSession();
        return true;
      }
      this.connectState = 'connecting';
      this._setMsg('正在连接服务端…', 'info');
      try {
        await net.connect(defaultWsUrl());
        this.connectState = 'open';
        if (this.resumeToken && !this.sessionActive) await this.tryResumeSession();
        if (!this.loggedIn) this._setMsg('已连接', 'success');
        return true;
      } catch (e) {
        this.connectState = 'closed';
        this._setMsg(e.message || '连接失败', 'error');
        return false;
      }
    },

    // 请求验证码；成功则回填 code（服务端下发文本，等价桌面端 placeholder 回显）。
    async refreshCaptcha() {
      try {
        const r = await account.requestCaptcha(net, this.clientId);
        if (r.status === 0) {
          this.captchaImage = r.image;
          this.captchaCooldown = r.remaining > 0 ? r.remaining : 60;
          this._setMsg('验证码已获取', 'success');
        } else if (r.status === -3013) {
          this.captchaCooldown = r.remaining > 0 ? r.remaining : 60;
          this._setMsg(`获取过于频繁，请 ${this.captchaCooldown} 秒后再试`, 'error');
        } else if (r.status === -3014) {
          this.captchaCooldown = r.remaining > 0 ? r.remaining : 600;
          this._setMsg('账号已锁定，请稍后再试', 'error');
        } else this._setMsg(account.describeAuthRet(r.status), 'error');
        return r;
      } catch (e) {
        this._setMsg(e.message || '获取验证码失败', 'error');
        return null;
      }
    },

    async login(username, password, captcha) {
      try {
        const r = await account.login(net, { username, password, captcha, clientId: this.clientId });
        if (r.ret === 0) {
          this.loggedIn = true;
          this.sessionActive = true;
          this.loginName = r.loginName || username;
          this.tokenUser = this.loginName;
          this.resumeToken = r.resumeToken || '';
          saveSessionValue('cloudpan.resumeToken', this.resumeToken);
          saveSessionValue('cloudpan.resumeUser', this.tokenUser);
          this.lastClose = null;
          this._setMsg(`登录成功：${this.loginName}`, 'success');
        } else {
          if (r.ret === -3014) this.captchaCooldown = r.remaining || 600;
          this._setMsg(account.describeAuthRet(r.ret), 'error');
        }
        return r;
      } catch (e) {
        this._setMsg(e.message || '登录失败', 'error');
        return { ret: null, error: e.message };
      }
    },

    async register(username, password, captcha) {
      try {
        const r = await account.register(net, { username, password, captcha, clientId: this.clientId });
        if (r.success) this._setMsg(`注册成功：${r.name || username}，请登录`, 'success');
        else {
          if (r.ret === -3014) this.captchaCooldown = r.remaining || 600;
          this._setMsg(account.describeAuthRet(r.ret), 'error');
        }
        return r;
      } catch (e) {
        this._setMsg(e.message || '注册失败', 'error');
        return { success: false, error: e.message };
      }
    },

    async logout() {
      this.init();
      if (this.sessionActive && net && net.connected) {
        try {
          const r = await account.sessionLogout(net, this.clientId);
          if (r.ret !== 0) {
            this._setMsg(account.describeAuthRet(r.ret), 'error');
            return false;
          }
        } catch (e) {
          this._setMsg(e.message || '退出登录失败', 'error');
          return false;
        }
      }
      this._clearSession();
      this._setMsg('已退出登录', 'info');
      return true;
    },

    async tryResumeSession() {
      if (!this.resumeToken || !this.clientId || !net || !net.connected) return false;
      this.resumePending = true;
      try {
        const r = await account.sessionResume(net, this.clientId, this.resumeToken);
        if (r.ret === 0) {
          this.loggedIn = true;
          this.sessionActive = true;
          this.loginName = r.username || this.tokenUser;
          this.tokenUser = this.loginName;
          saveSessionValue('cloudpan.resumeUser', this.tokenUser);
          this.lastClose = null;
          this._setMsg(`会话已恢复：${this.loginName}`, 'success');
          return true;
        }
        this._clearSession();
        this._setMsg(account.describeAuthRet(r.ret), 'error');
        return false;
      } catch (e) {
        this._clearSession();
        this._setMsg(e.message || '会话恢复失败，请重新登录', 'error');
        return false;
      } finally {
        this.resumePending = false;
      }
    },

    reconnect() {
      if (this.connectState === 'connecting') return Promise.resolve(false);
      return this.connect();
    },

    _clearSession() {
      this.loggedIn = false;
      this.sessionActive = false;
      this.loginName = '';
      this.resumeToken = '';
      this.tokenUser = '';
      this.captchaImage = '';
      this.captchaCooldown = 0;
      this.lastClose = null;
      saveSessionValue('cloudpan.resumeToken', '');
      saveSessionValue('cloudpan.resumeUser', '');
    },

    _handleInvalidated(pdu) {
      const event = parseInvalidatedEvent(pdu);
      this._clearSession();
      this._setMsg(account.describeAuthRet(event.reason), 'error');
    },
  },
});
