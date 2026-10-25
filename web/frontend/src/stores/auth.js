// 认证/连接 Store（Pinia）。NetClient 保持在模块作用域（非响应式），
// 避免 Vue 深度代理 WebSocket/定时器；Store 只保存可序列化的界面状态。
import { defineStore } from 'pinia';
import { defaultWsUrl } from '../protocol/client.js';
import { getNet } from '../protocol/net.js';
import * as account from '../protocol/commands/account.js';

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
  if (!/^[0-9a-f]{32}$/i.test(id)) {
    id = genClientId();
    try { localStorage.setItem('cloudpan.clientId', id); } catch { /* ignore */ }
  }
  return id;
}

export const useAuthStore = defineStore('auth', {
  state: () => ({
    clientId: '',
    connectState: 'idle', // idle | connecting | open | closed
    loggedIn: false,
    loginName: '',
    captchaCode: '',
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
        n.onOpen(() => { this.connectState = 'open'; });
        n.onClose((ev) => {
          this.connectState = 'closed';
          this.loggedIn = false;
          // 4001=上游连不上，4003=网关满载，其余按通用断线提示。
          const reason = ev && ev.code === 4001 ? '无法连接到服务端（上游 TCP 失败）'
            : ev && ev.code === 4003 ? '网关连接数已满'
            : '与服务端的连接已断开';
          this._setMsg(reason, 'error');
        });
      });
      return net;
    },
    init() {
      if (!this.clientId) this.clientId = loadClientId();
      this._ensureNet();
    },
    _setMsg(text, type = 'info') { this.message = text; this.messageType = type; },

    async connect() {
      this.init();
      this.connectState = 'connecting';
      this._setMsg('正在连接服务端…', 'info');
      try {
        await net.connect(defaultWsUrl());
        this.connectState = 'open';
        this._setMsg('已连接', 'success');
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
        const r = await account.requestCaptcha(net);
        if (r.status === 0) {
          this.captchaCode = r.code;
          this.captchaCooldown = r.remaining > 0 ? r.remaining : 60;
          this._setMsg('验证码已获取', 'success');
        } else if (r.status === -5) {
          this.captchaCooldown = r.remaining > 0 ? r.remaining : 60;
          this._setMsg(`获取过于频繁，请 ${this.captchaCooldown} 秒后再试`, 'error');
        } else if (r.status === -6) {
          this.captchaCooldown = r.remaining > 0 ? r.remaining : 600;
          this._setMsg('账号已锁定，请稍后再试', 'error');
        }
        return r;
      } catch (e) {
        this._setMsg(e.message || '获取验证码失败', 'error');
        return null;
      }
    },

    async login(username, password, captcha) {
      try {
        const r = await account.login(net, { username, password, captcha, clientId: this.clientId });
        if (r.ret === 1) {
          this.loggedIn = true;
          this.loginName = r.loginName || username;
          this._setMsg(`登录成功：${this.loginName}`, 'success');
        } else {
          if (r.ret === -4) this.captchaCooldown = r.remaining || 600;
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
        const r = await account.register(net, { username, password, captcha });
        if (r.success) this._setMsg(`注册成功：${r.name || username}，请登录`, 'success');
        else {
          if (r.ret === -4) this.captchaCooldown = r.remaining || 600;
          this._setMsg(account.describeAuthRet(r.ret), 'error');
        }
        return r;
      } catch (e) {
        this._setMsg(e.message || '注册失败', 'error');
        return { success: false, error: e.message };
      }
    },

    logout() {
      this.loggedIn = false;
      this.loginName = '';
      this.captchaCode = '';
      try { net && net.close(); } catch { /* ignore */ }
      this.connectState = 'closed';
      this._setMsg('已退出登录', 'info');
    },
  },
});
