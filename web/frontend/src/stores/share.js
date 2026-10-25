// 分享 Store：好友文件分享邀请（35 push → 36/37）与外链分享（52/53、54/55、56/57）。
// 37 无关联字段，故 36 必须串行：同一时刻只允许一个"接受"在途，其余邀请排队。
// 外链操作也按单飞处理（同一响应类型无法区分并发请求）。
// 外链取回自 M5b 起走服务端"条目优先"（对象文件零复制，物理文件仍复制），见记录 M5b 节。
import { defineStore } from 'pinia';
import { getNet } from '../protocol/net.js';
import { T } from '../protocol/msgtypes.js';
import * as shareCmd from '../protocol/commands/share.js';
import { useAuthStore } from './auth.js';
import { useDriveStore } from './drive.js';

function emptyLink() {
  return {
    name: '', absPath: '', relPath: '',
    exists: false, state: shareCmd.PUBLINK_STATE_VALID, link: '',
    loaded: false, busy: false,
  };
}

export const useShareStore = defineStore('share', {
  state: () => ({
    invites: [],          // 排队中的邀请 [{sharer, sourceAbsPath, fileName}]
    activeInvite: null,   // 当前弹窗展示的邀请
    accepting: false,
    initialized: false,
    hint: '',
    hintType: 'info',
    link: emptyLink(),    // 当前外链目标与结果
    resolveInput: '',
    resolveBusy: false,
    resolveResult: null,  // {ok, savedPath}
  }),
  getters: {
    pendingInviteCount: (s) => s.invites.length + (s.activeInvite ? 1 : 0),
    linkStateLabel: (s) => shareCmd.describePublishState(s.link.state),
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

    // ===== 好友文件分享 =====
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

    // ===== 外链分享 =====
    selectLinkTarget(file) {
      this.link = { ...emptyLink(), name: file.name, absPath: file.absPath };
      this.setHint('', 'info');
    },
    async fetchOrCreateLink() {
      const auth = useAuthStore();
      if (this.link.busy) return;
      if (!this.link.absPath) { this.setHint('请先选择要分享的文件', 'error'); return; }
      const rel = shareCmd.relativePathOf(this.link.absPath, useDriveStore().userRoot);
      if (!rel) { this.setHint('只能分享自己云盘内的文件', 'error'); return; }
      this.link.busy = true;
      try {
        const q = await shareCmd.queryPublishLink({ owner: auth.loginName, relativePath: rel });
        if (q.exists) {
          this.link = { ...this.link, relPath: rel, exists: true, state: q.state, link: q.link, loaded: true };
          this._hintForState(q.state, '已获取现有分享链接');
          return;
        }
        const c = await shareCmd.createPublishLink({ relativePath: rel });
        if (!c.ok || !c.link) { this.setHint('生成分享链接失败', 'error'); return; }
        this.link = { ...this.link, relPath: rel, exists: true, state: c.state, link: c.link, loaded: true };
        this._hintForState(c.state, '已生成分享链接（默认 24 小时有效）');
      } catch (e) {
        this.setHint(e.message || '获取分享链接失败', 'error');
      } finally {
        this.link.busy = false;
      }
    },
    async regenerateLink() {
      if (this.link.busy || !this.link.relPath) return;
      this.link.busy = true;
      try {
        const c = await shareCmd.createPublishLink({
          relativePath: this.link.relPath, action: shareCmd.PUBLINK_ACTION_NEW,
        });
        if (!c.ok || !c.link) { this.setHint('重新生成链接失败', 'error'); return; }
        this.link = { ...this.link, exists: true, state: c.state, link: c.link, loaded: true };
        this.setHint('已重新生成链接，旧链接立即失效', 'success');
      } catch (e) {
        this.setHint(e.message || '重新生成链接失败', 'error');
      } finally {
        this.link.busy = false;
      }
    },
    async renewLink(days) {
      if (this.link.busy || !this.link.relPath) return;
      const d = Number(days) > 0 ? Number(days) : shareCmd.PUBLINK_RENEW_DEFAULT_DAYS;
      this.link.busy = true;
      try {
        const c = await shareCmd.createPublishLink({
          relativePath: this.link.relPath,
          action: shareCmd.PUBLINK_ACTION_RENEW,
          seconds: d * 24 * 3600,
        });
        if (!c.ok || !c.link) { this.setHint('续期失败（链接已被撤销或记录不存在）', 'error'); return; }
        this.link = { ...this.link, exists: true, state: c.state, link: c.link, loaded: true };
        this.setHint(`已续期 ${d} 天，链接文本不变`, 'success');
      } catch (e) {
        this.setHint(e.message || '续期失败', 'error');
      } finally {
        this.link.busy = false;
      }
    },
    _hintForState(state, prefix) {
      if (state === shareCmd.PUBLINK_STATE_VALID) { this.setHint(prefix, 'success'); return; }
      const label = shareCmd.describePublishState(state);
      const advice = state === shareCmd.PUBLINK_STATE_REVOKED
        ? '只能重新生成链接'
        : '可续期或重新生成';
      this.setHint(`${prefix}，但链接${label}：${advice}`, 'error');
    },
    async resolveLink() {
      if (this.resolveBusy) return;
      const token = shareCmd.tokenFromLink(this.resolveInput);
      if (!token) { this.setHint('请粘贴分享链接或 token', 'error'); return; }
      this.resolveBusy = true;
      this.resolveResult = null;
      try {
        const r = await shareCmd.resolvePublishLink(token);
        this.resolveResult = r;
        if (r.ok) {
          this.setHint(`已保存到 ${r.savedPath || shareCmd.PUBLINK_SAVE_DIR}`, 'success');
          await this._refreshDrive();
        } else {
          this.setHint('分享链接无效或已失效（不存在/已撤销/已过期）', 'error');
        }
      } catch (e) {
        this.setHint(e.message || '获取分享文件失败', 'error');
      } finally {
        this.resolveBusy = false;
      }
    },

    reset() {
      this.invites = [];
      this.activeInvite = null;
      this.accepting = false;
      this.hint = '';
      this.hintType = 'info';
      this.link = emptyLink();
      this.resolveInput = '';
      this.resolveBusy = false;
      this.resolveResult = null;
    },
  },
});
