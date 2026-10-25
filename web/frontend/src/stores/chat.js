// 聊天 Store：会话消息、未读计数、历史拉取（17→18）与实时 push（15）。
// 设计说明（对齐变更记录 M2a）：
//  - 发送即忘（服务端不回响应），本地回显自己的消息（桌面端不回显，属有意增强）；
//  - 17/18 为单向"对方→我"未投递补发，发送者必为对方 → 按 peer 气泡显示并标注 redelivered；
//  - openChat 用补发收件箱整体替换会话，并对拉取期间到达的实时消息按 (text,time) 去重合并。
import { defineStore } from 'pinia';
import { getNet } from '../protocol/net.js';
import { T } from '../protocol/msgtypes.js';
import * as chatCmd from '../protocol/commands/chat.js';
import { useAuthStore } from './auth.js';

export const useChatStore = defineStore('chat', {
  state: () => ({
    conversations: {}, // peer -> [{ from: 'me'|'peer'|'history', text, time }]
    unread: {},        // peer -> 未读数
    activePeer: '',
    initialized: false,
  }),
  getters: {
    activeMessages: (s) => (s.activePeer ? s.conversations[s.activePeer] || [] : []),
    totalUnread: (s) => Object.values(s.unread).reduce((a, b) => a + b, 0),
  },
  actions: {
    init() {
      if (this.initialized) return;
      this.initialized = true;
      getNet().onPush(T.CHAT_REQUEST, (pdu) => {
        const { from, text, time } = chatCmd.parseIncomingChat(pdu);
        if (!from) return;
        this._append(from, { from: 'peer', text, time });
        if (this.activePeer !== from) {
          this.unread = { ...this.unread, [from]: (this.unread[from] || 0) + 1 };
        }
      });
    },
    _append(peer, msg) {
      const list = this.conversations[peer] ? [...this.conversations[peer]] : [];
      list.push(msg);
      this.conversations = { ...this.conversations, [peer]: list };
    },

    // 打开与 peer 的会话：先置 activePeer（此后 push 不再计未读），再拉补发收件箱并去重合并。
    // 17/18 只返回"对方→我"的未投递消息（单向查询），发送者必为对方 → 按 peer 气泡正常
    // 显示并标记 redelivered（用户验收决定 2026-10-24：不用灰色居中的中性历史样式）。
    async openChat(peer) {
      const auth = useAuthStore();
      this.activePeer = peer;
      this.unread = { ...this.unread, [peer]: 0 };
      const liveBefore = this.conversations[peer] || [];
      try {
        const history = await chatCmd.fetchChatHistory(auth.loginName, peer);
        const histRecords = history.map((h) => ({ from: 'peer', text: h.text, time: h.time, redelivered: true }));
        const key = (m) => `${m.time}\u0000${m.text}`;
        const histKeys = new Set(history.map((h) => `${h.time}\u0000${h.text}`));
        // 拉取期间/之前到达的实时消息或上一次补发：收件箱里没有的才保留，避免重复。
        const liveOnly = liveBefore.filter((m) => !histKeys.has(key(m)));
        this.conversations = { ...this.conversations, [peer]: [...histRecords, ...liveOnly] };
      } catch {
        // 补发拉取失败不阻断实时聊天：保留已有实时消息。
        if (!this.conversations[peer]) this.conversations = { ...this.conversations, [peer]: liveBefore };
      }
    },

    // 发送消息到当前会话；正文超限等错误抛给 UI 提示。
    send(text) {
      const auth = useAuthStore();
      const peer = this.activePeer;
      const body = (text || '').trim();
      if (!peer || !body) return false;
      chatCmd.sendChat(auth.loginName, peer, body); // 可能抛"消息过长"
      this._append(peer, { from: 'me', text: body, time: chatCmd.formatQtDateTime() });
      return true;
    },

    // 删好友后由 friends store 调用：服务端已联动清除双方记录，本地同步清空。
    dropConversation(peer) {
      const conv = { ...this.conversations }; delete conv[peer];
      const unread = { ...this.unread }; delete unread[peer];
      this.conversations = conv;
      this.unread = unread;
      if (this.activePeer === peer) this.activePeer = '';
    },
    closeChat() { this.activePeer = ''; },
    reset() { this.conversations = {}; this.unread = {}; this.activePeer = ''; },
  },
});
