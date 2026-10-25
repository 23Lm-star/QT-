// 群文本 Store：选中群的消息列表、发送（88/89 乐观+确认）、实时 push（90）、
// 按游标拉取历史/翻页（91/92）、已读确认（93/94，节流）。
// 游标（read/water/first，BigInt）保存在模块级 Map，不进响应式 state；消息列表响应式。
// 对齐 Client/groupchat.cpp + groupchatstate.h：seq 去重、mode1 向后翻页、
// 发送幂等键 client_msg_id（失败双击重发复用同键）。
import { defineStore } from 'pinia';
import { getNet } from '../protocol/net.js';
import { T } from '../protocol/msgtypes.js';
import * as g from '../protocol/commands/group.js';
import { useAuthStore } from './auth.js';
import { useGroupStore } from './group.js';

// gidStr -> { read, water, first, loaded }（全 BigInt）
const cursors = new Map();
const cur = (gid) => {
  const k = String(gid);
  if (!cursors.has(k)) cursors.set(k, { read: 0n, water: 0n, first: 0n, loaded: false });
  return cursors.get(k);
};

let reportTimers = {}; // gidStr -> timeout id（已读确认节流 400ms）

export const useGroupChatStore = defineStore('groupChat', {
  state: () => ({
    activeId: null,     // 当前选中群（BigInt）
    messages: {},       // gidStr -> [{seq(string), sender, content, msgType, mine, state, clientMsgId, time}]
    initialized: false,
    loading: false,
  }),
  getters: {
    activeMessages: (s) => (s.activeId != null ? s.messages[String(s.activeId)] || [] : []),
  },
  actions: {
    init() {
      if (this.initialized) return;
      this.initialized = true;
      g.bindGroup(getNet());
      getNet().onPush(T.GROUP_MESSAGE_EVENT, (pdu) => this._onMessageEvent(pdu));
    },
    _list(gid) {
      const k = String(gid);
      if (!this.messages[k]) this.messages = { ...this.messages, [k]: [] };
      return this.messages[k];
    },
    _setList(gid, arr) { this.messages = { ...this.messages, [String(gid)]: arr }; },

    // 打开选中群：加载最近历史（mode1 向后，cursor=大值），并确认已读到最新。
    async openGroup(gid) {
      this.init();
      this.activeId = gid;
      const c = cur(gid);
      if (c.loaded) return;
      this.loading = true;
      try {
        const r = await g.pullGroupMessages(gid, 1, (1n << 63n) - 1n, 50);
        if (r.code === 0) {
          this._setList(gid, r.records.map((rec) => this._toMsg(rec, gid)));
          if (r.records.length) {
            c.first = r.records[0].seq;
            c.water = r.records[r.records.length - 1].seq;
            c.read = c.water;
            this._report(gid, true);
          }
          c.loaded = true;
        }
      } catch { /* 历史加载失败不阻断，后续 push/翻页可恢复 */ }
      this.loading = false;
    },

    // 翻页加载更早消息（mode1，cursor = firstSeq-1）。
    async loadOlder() {
      const gid = this.activeId;
      if (gid == null) return;
      const c = cur(gid);
      if (c.first <= 1n) return;
      try {
        const r = await g.pullGroupMessages(gid, 1, c.first - 1n, 50);
        if (r.code === 0 && r.records.length) {
          const older = r.records.map((rec) => this._toMsg(rec, gid));
          c.first = r.records[0].seq;
          this._setList(gid, [...older, ...(this.messages[String(gid)] || [])]);
        }
      } catch { /* ignore */ }
    },

    _toMsg(rec, gid) {
      const auth = useAuthStore();
      return {
        seq: String(rec.seq),
        sender: rec.sender,
        content: rec.content,
        msgType: rec.msgType,
        mine: rec.sender === auth.loginName,
        state: 'sent',
        createdAt: rec.createdAt,
        time: this._fmt(rec.createdAt),
      };
    },
    _fmt(ms) {
      const d = new Date(Number(ms));
      const p = (n) => String(n).padStart(2, '0');
      return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
    },

    // push 90：在线到达。按 seq 去重；自己的消息合并进乐观项（90 可能先于 89 到达）。
    // 更新水位，调度已读确认。
    _onMessageEvent(pdu) {
      let ev;
      try { ev = g.parseGroupMessageEvent(pdu); } catch { return; }
      const gid = ev.groupId;
      const isActive = this.activeId != null && String(this.activeId) === String(gid);
      const seqStr = String(ev.seq);
      const auth = useAuthStore();
      const mine = ev.sender === auth.loginName;
      const list = [...(this.messages[String(gid)] || [])];
      const seqExists = list.some((m) => m.seq === seqStr);
      if (isActive) {
        if (!seqExists) {
          // 自己的消息：优先把已有乐观项（seq='' 且同内容且未落定）补上真实 seq，避免与 89 双份。
          const optIdx = mine
            ? list.findIndex((m) => m.mine && m.seq === '' && m.content === ev.content && m.state !== 'sent')
            : -1;
          if (optIdx >= 0) list[optIdx] = { ...list[optIdx], seq: seqStr, state: 'sent', msgId: String(ev.msgId) };
          else list.push(this._toMsg(ev, gid));
          this._setList(gid, list);
        }
      }
      const c = cur(gid);
      if (BigInt(ev.seq) > c.water) c.water = BigInt(ev.seq);
      if (isActive) {
        c.read = c.water;
        this._report(gid, false);
      }
      // 非活跃群：刷新群列表未读徽章（服务端 96 权威）。
      useGroupStore().refreshUnread();
    },

    // 发送群消息：先乐观插入"发送中"，88 成功后按 seq 落定，失败标"发送失败"。
    async send(content) {
      const gid = this.activeId;
      const body = (content || '').trim();
      if (gid == null || !body) return false;
      const auth = useAuthStore();
      const clientMsgId = g.newClientMsgId();
      const list = [...(this.messages[String(gid)] || [])];
      const optimistic = {
        seq: '', sender: auth.loginName, content: body, msgType: 0, mine: true,
        state: 'sending', clientMsgId, time: this._fmt(BigInt(Date.now())),
      };
      list.push(optimistic);
      this._setList(gid, list);
      try {
        const r = await g.sendGroupMessage(gid, clientMsgId, body);
        const arr = [...(this.messages[String(gid)] || [])];
        const idx = arr.findIndex((m) => m.clientMsgId === clientMsgId);
        if (idx >= 0) {
          if (r.code === 0) {
            arr[idx] = { ...arr[idx], seq: String(r.seq), state: 'sent', msgId: String(r.msgId) };
            const c = cur(gid);
            if (r.seq > c.water) c.water = r.seq;
            c.read = c.water; // 自己发的视为已读
          } else {
            arr[idx] = { ...arr[idx], state: 'failed', error: g.describeGroupCode(r.code) };
          }
          this._setList(gid, arr);
        }
        return r.code === 0;
      } catch (e) {
        const arr = [...(this.messages[String(gid)] || [])];
        const idx = arr.findIndex((m) => m.clientMsgId === clientMsgId);
        if (idx >= 0) { arr[idx] = { ...arr[idx], state: 'failed', error: e.message }; this._setList(gid, arr); }
        return false;
      }
    },

    // 重发失败消息：复用原 client_msg_id（幂等），仅当确实未入库时生效。
    async retry(clientMsgId) {
      const gid = this.activeId;
      const arr = [...(this.messages[String(gid)] || [])];
      const m = arr.find((x) => x.clientMsgId === clientMsgId);
      if (!m || m.state !== 'failed') return false;
      m.state = 'sending';
      this._setList(gid, arr);
      try {
        const r = await g.sendGroupMessage(gid, clientMsgId, m.content);
        const arr2 = [...(this.messages[String(gid)] || [])];
        const it = arr2.find((x) => x.clientMsgId === clientMsgId);
        if (it) {
          if (r.code === 0) it.state = 'sent', it.seq = String(r.seq);
          else it.state = 'failed', it.error = g.describeGroupCode(r.code);
          this._setList(gid, arr2);
        }
        return r.code === 0;
      } catch (e) {
        const arr2 = [...(this.messages[String(gid)] || [])];
        const it = arr2.find((x) => x.clientMsgId === clientMsgId);
        if (it) { it.state = 'failed'; it.error = e.message; this._setList(gid, arr2); }
        return false;
      }
    },

    // 已读确认：立即或节流（400ms）发送 93。
    _report(gid, immediate) {
      const k = String(gid);
      const doSend = async () => {
        reportTimers[k] = null;
        try { await g.confirmGroupRead(gid, cur(gid).read); } catch { /* ignore */ }
      };
      if (immediate) { if (reportTimers[k]) { clearTimeout(reportTimers[k]); reportTimers[k] = null; } doSend(); return; }
      if (reportTimers[k]) return;
      reportTimers[k] = setTimeout(doSend, 400);
    },

    reset() {
      this.messages = {};
      this.activeId = null;
      cursors.clear();
      for (const k of Object.keys(reportTimers)) { if (reportTimers[k]) clearTimeout(reportTimers[k]); }
      reportTimers = {};
    },
  },
});
