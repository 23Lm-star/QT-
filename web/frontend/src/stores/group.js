// 群 Store：群列表、成员、我的邀请、未读徽章与全部生命周期操作（命令 60-87）。
// 请求-响应关联由 protocol/commands/group.js 的 request_id 引擎完成；本 store 只在
// init 时挂 push 84（成员变化）/85（邀请事件），变化即刷新对应列表。
// 群文本（88-96）在 stores/groupchat.js，群文件（99-106）在 GroupFilePanel + transfer store。
import { defineStore } from 'pinia';
import { getNet } from '../protocol/net.js';
import { T } from '../protocol/msgtypes.js';
import * as g from '../protocol/commands/group.js';
import { useAuthStore } from './auth.js';

const idStr = (id) => (id == null ? '' : String(id));

export const useGroupStore = defineStore('group', {
  state: () => ({
    groups: [],           // [{groupId,name,status,role,version}]
    selectedId: null,     // BigInt | null
    members: [],          // 选中群成员 [{name,role,status}]
    invitations: [],      // 我的待处理邀请 [{invitationId,groupId,groupName,inviter,status,expiresAt}]
    unread: {},           // String(groupId) -> number 未读数
    showInvites: false,   // 邀请弹窗
    hint: '',
    hintType: 'info',
    initialized: false,
    busy: false,
  }),
  getters: {
    selectedGroup: (s) => s.groups.find((grp) => grp.groupId === s.selectedId) || null,
    myRole() { return this.selectedGroup ? this.selectedGroup.role : ''; },
    isOwner() { return this.myRole === 'owner'; },
    isAdmin() { return this.myRole === 'owner' || this.myRole === 'admin'; },
    inviteCount: (s) => s.invitations.length,
  },
  actions: {
    // 挂载时调用（MainView）。挂 push 84/85 + 初始拉群列表/邀请/未读。整个会话只挂一次。
    init() {
      g.bindGroup(getNet());
      if (this.initialized) { this.refreshAll(); return; }
      this.initialized = true;
      const net = getNet();
      net.onPush(T.GROUP_MEMBER_EVENT, () => {
        this.refreshGroups();
        this.refreshUnread();
        if (this.selectedId != null) this.loadMembers();
      });
      net.onPush(T.GROUP_INVITATION_EVENT, () => this.refreshInvitations());
      this.refreshAll();
    },
    refreshAll() {
      this.refreshGroups();
      this.refreshInvitations();
      this.refreshUnread();
    },
    setHint(text, type = 'info') { this.hint = text; this.hintType = type; },

    // ===== 查询刷新 =====
    async refreshGroups() {
      try {
        const r = await g.listGroups();
        if (r.code !== 0) { this.setHint(g.describeGroupCode(r.code), 'error'); return; }
        // 服务端 list_groups 会返回 status='dissolved' 的历史行，群列表只展示活跃群。
        this.groups = r.groups.filter((grp) => grp.status === 'active');
        // 选中群若已不在列表（退出/解散/被移出），清除选择。
        if (this.selectedId != null && !this.groups.some((grp) => grp.groupId === this.selectedId)) {
          this.selectedId = null;
          this.members = [];
        }
      } catch (e) { this.setHint(e.message || '获取群列表失败', 'error'); }
    },
    async refreshInvitations() {
      try {
        const r = await g.queryInvitations();
        if (r.code !== 0) { this.setHint(g.describeGroupCode(r.code), 'error'); return; }
        this.invitations = r.invitations;
      } catch (e) { this.setHint(e.message || '获取群邀请失败', 'error'); }
    },
    async refreshUnread() {
      try {
        const r = await g.queryGroupUnread();
        if (r.code !== 0) return;
        const map = {};
        for (const it of r.items) map[idStr(it.groupId)] = Number(it.unread);
        this.unread = map;
      } catch { /* 未读摘要失败不弹提示（后台轮询项） */ }
    },
    async loadMembers() {
      if (this.selectedId == null) { this.members = []; return; }
      try {
        const r = await g.listGroupMembers(this.selectedId);
        if (r.code !== 0) { this.setHint(g.describeGroupCode(r.code), 'error'); return; }
        this.members = r.members;
      } catch (e) { this.setHint(e.message || '获取群成员失败', 'error'); }
    },

    // ===== 选择 =====
    selectGroup(groupId) {
      this.selectedId = groupId;
      this.loadMembers();
    },

    // ===== 生命周期操作 =====
    async createGroup(name) {
      const r = await this._op(() => g.createGroup(name));
      if (r && r.code === 0) { await this.refreshGroups(); this.selectGroup(r.groupId); }
      return r && r.code === 0;
    },
    async inviteMember(targetName) {
      if (this.selectedId == null) { this.setHint('请先选择群', 'error'); return false; }
      const r = await this._op(() => g.inviteMember(this.selectedId, targetName, g.newInviteKey()));
      return r && r.code === 0;
    },
    async acceptInvitation(invitationId) {
      const r = await this._op(() => g.acceptInvitation(invitationId));
      if (r && r.code === 0) { await this.refreshInvitations(); await this.refreshGroups(); }
      return r && r.code === 0;
    },
    async rejectInvitation(invitationId) {
      const r = await this._op(() => g.rejectInvitation(invitationId));
      if (r && r.code === 0) await this.refreshInvitations();
      return r && r.code === 0;
    },
    async leaveGroup(groupId) {
      const r = await this._op(() => g.leaveGroup(groupId));
      if (r && r.code === 0) {
        if (this.selectedId === groupId) { this.selectedId = null; this.members = []; }
        await this.refreshGroups();
      }
      return r && r.code === 0;
    },
    async dissolveGroup(groupId) {
      const r = await this._op(() => g.dissolveGroup(groupId));
      if (r && r.code === 0) {
        if (this.selectedId === groupId) { this.selectedId = null; this.members = []; }
        await this.refreshGroups();
      }
      return r && r.code === 0;
    },
    async removeMember(targetName) {
      const r = await this._op(() => g.removeMember(this.selectedId, targetName));
      if (r && r.code === 0) await this.loadMembers();
      return r && r.code === 0;
    },
    async setMemberRole(targetName, role) {
      const r = await this._op(() => g.setGroupRole(this.selectedId, targetName, role));
      if (r && r.code === 0) await this.loadMembers();
      return r && r.code === 0;
    },
    async transferGroupOwner(targetName) {
      const r = await this._op(() => g.transferOwner(this.selectedId, targetName));
      if (r && r.code === 0) { await this.loadMembers(); await this.refreshGroups(); }
      return r && r.code === 0;
    },

    // 统一操作包装：捕获异常、把非 0 code 转成中文提示，返回解析结果。
    async _op(fn) {
      if (this.busy) { this.setHint('操作进行中，请稍候…', 'info'); return null; }
      this.busy = true;
      try {
        const r = await fn();
        if (r && r.code !== 0) this.setHint(g.describeGroupCode(r.code) || `操作失败（code=${r.code}）`, 'error');
        return r;
      } catch (e) {
        this.setHint(e.message || '群操作失败', 'error');
        return { code: null, error: e.message };
      } finally {
        this.busy = false;
      }
    },

    unreadFor(groupId) { return this.unread[idStr(groupId)] || 0; },
    reset() {
      this.groups = [];
      this.selectedId = null;
      this.members = [];
      this.invitations = [];
      this.unread = {};
      this.showInvites = false;
      this.hint = '';
      this.busy = false;
    },
  },
});
