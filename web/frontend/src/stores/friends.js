// 好友 Store：好友列表、在线用户、加好友申请（push 9）与结果（push 10）。
// 加好友无可靠直回响应（见 protocol/commands/friend.js 头注释），全部经 push 事件驱动。
import { defineStore } from 'pinia';
import { getNet } from '../protocol/net.js';
import { T } from '../protocol/msgtypes.js';
import * as friendCmd from '../protocol/commands/friend.js';
import { useAuthStore } from './auth.js';
import { useChatStore } from './chat.js';

export const useFriendsStore = defineStore('friends', {
  state: () => ({
    friends: [],
    onlineUsers: [],        // REQ-PAGE-FETCH-001：{name, online} 条目数组
    onlineNames: [],        // 名字数组（ShareView 等旧消费点）
    incomingRequest: null, // { requester } 待处理的好友申请（push 9）
    hint: '',
    hintType: 'info',
    initialized: false,
    syncTimer: null,       // 好友列表同步轮询（协议无"被删好友"推送，被删方靠轮询+聚焦刷新感知）
    _focusHandler: null,
  }),
  actions: {
    // 在共享 NetClient 上挂 push 处理器；整个会话只挂一次（重连不清处理器）。
    init() {
      if (this.initialized) return;
      this.initialized = true;
      const net = getNet();
      net.onPush(T.ADDFRIEND_REQUEST, (pdu) => {
        const { requester } = friendCmd.parseIncomingFriendRequest(pdu);
        if (requester) this.incomingRequest = { requester };
      });
      net.onPush(T.ADDFRIEND_RESPOND, (pdu) => {
        const { ret } = friendCmd.parseFriendResult(pdu);
        this.setHint(friendCmd.describeFriendRet(ret), ret >= 0 ? 'success' : 'error');
        if (ret === 0) this.loadFriends();
      });
    },
    setHint(text, type = 'info') { this.hint = text; this.hintType = type; },

    // 好友列表同步：10s 轮询 + 窗口聚焦时刷新（MainView 挂载时启动，登出/卸载时停止）。
    startSync() {
      if (this.syncTimer) return;
      this.syncTimer = setInterval(() => this.syncFriends(), 10000);
      this._focusHandler = () => this.syncFriends();
      window.addEventListener('focus', this._focusHandler);
    },
    stopSync() {
      if (this.syncTimer) { clearInterval(this.syncTimer); this.syncTimer = null; }
      if (this._focusHandler) { window.removeEventListener('focus', this._focusHandler); this._focusHandler = null; }
    },
    // 拉最新好友列表；若当前会话对象已不是好友（对方删除了自己），关闭会话并提示。
    async syncFriends() {
      await this.loadFriends();
      const chat = useChatStore();
      if (chat.activePeer && !this.friends.includes(chat.activePeer)) {
        const removed = chat.activePeer;
        chat.dropConversation(removed);
        this.setHint(`好友列表已同步：${removed} 已不是你的好友，会话已关闭`, 'info');
      }
    },

    async loadFriends() {
      try { this.friends = await friendCmd.fetchFriends(); }
      catch (e) { this.setHint(e.message || '获取好友列表失败', 'error'); }
    },
    async loadOnlineUsers() {
      const auth = useAuthStore();
      try {
        // REQ-PAGE-FETCH-001：条目含在线状态；onlineNames 供旧消费点按名匹配
        const entries = await friendCmd.fetchOnlineUsers(auth.loginName);
        this.onlineUsers = entries;
        this.onlineNames = entries.map((e) => e.name);
      } catch (e) { this.setHint(e.message || '获取在线用户失败', 'error'); }
    },
    // 发送好友申请（9）：发送即忘，结果经 push 10 异步到达。
    addFriend(target) {
      const auth = useAuthStore();
      try {
        friendCmd.sendAddFriendRequest(auth.loginName, target);
        this.setHint(`已向 ${target} 发送好友请求，等待对方同意…`, 'info');
      } catch (e) { this.setHint(e.message || '发送失败', 'error'); }
    },
    // 同意申请（10）：服务端会把结果 push 给双方。
    agreeAdd() {
      const auth = useAuthStore();
      if (!this.incomingRequest) return;
      try {
        friendCmd.sendAddFriendAgree(auth.loginName, this.incomingRequest.requester);
        this.setHint(`已同意 ${this.incomingRequest.requester} 的好友申请`, 'info');
      } catch (e) { this.setHint(e.message || '发送失败', 'error'); }
      this.incomingRequest = null;
    },
    // 拒绝：无协议消息（与桌面端一致），仅关闭提示。
    declineAdd() { this.incomingRequest = null; },

    async removeFriend(target) {
      const auth = useAuthStore();
      try {
        const ok = await friendCmd.deleteFriend(target, auth.loginName);
        if (ok) {
          this.setHint(`已删除好友 ${target}（双方私聊记录已由服务端清除）`, 'success');
          this.friends = this.friends.filter((n) => n !== target);
          useChatStore().dropConversation(target);
        } else {
          this.setHint('删除好友失败', 'error');
        }
        return ok;
      } catch (e) { this.setHint(e.message || '删除好友失败', 'error'); return false; }
    },

    reset() {
      this.stopSync();
      this.friends = [];
      this.onlineUsers = [];
      this.onlineNames = [];
      this.incomingRequest = null;
      this.hint = '';
      this.hintType = 'info';
    },
  },
});
