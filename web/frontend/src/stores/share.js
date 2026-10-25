// 分享 Store（M5 本期范围）：好友文件分享邀请（35 push → 36/37）。
// 37 无关联字段，故 36 必须串行：同一时刻只允许一个"接受"在途，其余邀请排队。
// 外链分享（52-57）待服务端修复对象存储文件的取回路径后再接入，见变更记录 M5 阻塞节。
import { defineStore } from 'pinia';
import { getNet } from '../protocol/net.js';
import { T } from '../protocol/msgtypes.js';
import * as shareCmd from '../protocol/commands/share.js';
import { useAuthStore } from './auth.js';
import { useDriveStore } from './drive.js';

export const useShareStore = defineStore('share', {
  state: () => ({
    invites: [],          // 排队中的邀请 [{sharer, sourceAbsPath, fileName}]
    activeInvite: null,   // 当前弹窗展示的邀请
    accepting: false,
    initialized: false,
    hint: '',
    hintType: 'info',
  }),
  getters: {
    pendingInviteCount: (s) => s.invites.length + (s.activeInvite ? 1 : 0),
  },
  actions: {
    // MainView 挂载时调用；整个会话只挂一次 push 处理器（重连不清）。
    init() {
      if (this.initialized) return;
      this.initialized = true;
      getNet().onPush(T.SHAREFILE_AGREE_REQUEST, (pdu) => {
        const inv = shareCmd.parseShareInvite(pdu);
        if (!inv.sharer || !inv.sourceAbsPath || !inv.fileName) return;
        if (this.activeInvite) this.invites.push(inv);
        else this.activeInvite = inv;
      });
    },
    setHint(text, type = 'info') { this.hint = text; this.hintType = type; },

    async _refreshDrive() {
      const drv = useDriveStore();
      if (drv.curPath) await drv.refresh();
    },

    async sendToFriends({ absPath, receivers }) {
      const auth = useAuthStore();
      if (!absPath) { this.setHint('请先选择要分享的文件', 'error'); return false; }
      if (!receivers || receivers.length === 0) { this.setHint('请至少选择一位好友', 'error'); return false; }
      try {
        await shareCmd.sendShareInvites({
          sharerName: auth.loginName,
          receivers,
          sourceAbsPath: absPath,
        });
        this.setHint(`已向 ${receivers.length} 位好友发出分享邀请（仅在线好友能收到）`, 'success');
        return true;
      } catch (e) {
        this.setHint(e.message || '发送分享邀请失败', 'error');
        return false;
      }
    },
    async acceptActiveInvite() {
      if (!this.activeInvite || this.accepting) return;
      const auth = useAuthStore();
      const inv = this.activeInvite;
      this.accepting = true;
      try {
        const ok = await shareCmd.acceptShare({
          receiverName: auth.loginName,
          sourceAbsPath: inv.sourceAbsPath,
        });
        if (ok) {
          this.setHint(`已接收 ${inv.sharer} 分享的「${inv.fileName}」，文件已保存到你的云盘根目录（重名自动改名）`, 'success');
          await this._refreshDrive();
        } else {
          this.setHint(`接收「${inv.fileName}」失败：分享已失效、源文件不存在或重复接受`, 'error');
        }
      } catch (e) {
        this.setHint(e.message || '接收分享失败', 'error');
      } finally {
        this.accepting = false;
        this.activeInvite = this.invites.shift() || null;
      }
    },
    declineActiveInvite() {
      if (this.accepting) return;
      this.activeInvite = this.invites.shift() || null;
    },

    reset() {
      this.invites = [];
      this.activeInvite = null;
      this.accepting = false;
      this.hint = '';
      this.hintType = 'info';
    },
  },
});
