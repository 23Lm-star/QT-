// 云盘 Store：当前目录、条目列表与目录操作（19/21/23/25/27/38/58）。
// 路径体系（M3a 核实）：浏览/增删改用"服务端路径字面量"（root 默认 ./filesys，
// 与服务端配置第 3 行一致）；移动用用户根相对路径。
import { defineStore } from 'pinia';
import * as drive from '../protocol/commands/drive.js';
import { useTransferStore } from './transfer.js';

export const useDriveStore = defineStore('drive', {
  state: () => ({
    userRoot: '',
    curPath: '',
    entries: [],       // [{name,isDir}]
    loading: false,
    message: '',
    messageType: 'info',
    moveSrc: null,     // 移动对话框：源条目
    moveDirs: [],      // 移动对话框：当前相对目录的子目录名
    moveDirRel: '',    // 移动对话框：当前浏览的相对目录（'' = 用户根）
  }),
  getters: {
    // 面包屑：相对用户根的分段（含根项）。
    crumbs: (s) => {
      if (!s.userRoot || !s.curPath.startsWith(s.userRoot)) return [{ label: s.curPath, abs: s.curPath }];
      const rel = s.curPath.slice(s.userRoot.length).replace(/^\/+/, '');
      const out = [{ label: '根目录', abs: s.userRoot }];
      if (rel) {
        let acc = s.userRoot;
        for (const seg of rel.split('/')) {
          acc = `${acc}/${seg}`;
          out.push({ label: seg, abs: acc });
        }
      }
      return out;
    },
  },
  actions: {
    _msg(text, type = 'info') { this.message = text; this.messageType = type; },

    // 登录后调用：以登录名派生用户根并首次刷新。
    async enter(loginName) {
      this.userRoot = drive.userRoot(loginName);
      this.curPath = this.userRoot;
      await this.refresh();
    },

    async refresh() {
      if (!this.curPath) return;
      this.loading = true;
      try {
        this.entries = await drive.listDirectory(this.curPath);
      } catch (e) {
        this._msg(e.message || '列目录失败', 'error');
      } finally {
        this.loading = false;
      }
    },

    // 双击目录进入（与桌面端一致：拼路径后直接刷新，不发 21）。
    async enterDir(name) {
      this.curPath = drive.joinPath(this.curPath, name);
      await this.refresh();
    },
    async gotoCrumb(abs) {
      this.curPath = abs;
      await this.refresh();
    },
    async back() {
      if (!this.curPath || this.curPath === this.userRoot) return;
      const idx = this.curPath.lastIndexOf('/');
      if (idx <= 0) return;
      this.curPath = this.curPath.slice(0, idx);
      await this.refresh();
    },
    // 手动跳转（21→22）：服务端回显可进入目录；越权/不存在会被拒。
    async chdirTo(inputPath) {
      const target = (inputPath || '').trim();
      if (!target) return;
      try {
        const r = await drive.changeDirectory(target);
        if (!r.exists) { this._msg('目录不存在或无权访问', 'error'); return; }
        this.curPath = r.path;
        await this.refresh();
        this._msg('已跳转', 'success');
      } catch (e) { this._msg(e.message || '跳转失败', 'error'); }
    },

    async mkdir(name) {
      if (!drive.isValidFileName(name)) { this._msg('文件夹名不合法（≤31 字节，不含特殊字符）', 'error'); return false; }
      try {
        const ok = await drive.makeDirectory(this.curPath, name);
        this._msg(ok ? `已创建文件夹 ${name}` : '创建失败：目录已存在或无权创建', ok ? 'success' : 'error');
        if (ok) await this.refresh();
        return ok;
      } catch (e) { this._msg(e.message || '创建失败', 'error'); return false; }
    },

    async remove(entry) {
      const abs = drive.joinPath(this.curPath, entry.name);
      try {
        const ok = await drive.deleteEntry(abs, entry.isDir ? drive.TYPE_DIR : drive.TYPE_FILE);
        this._msg(ok ? `已删除 ${entry.name}${entry.isDir ? '（含子树，分享链接已联动清理）' : ''}` : '删除失败', ok ? 'success' : 'error');
        if (ok) await this.refresh();
        return ok;
      } catch (e) { this._msg(e.message || '删除失败', 'error'); return false; }
    },

    async rename(entry, newName) {
      if (newName === entry.name) return false;
      if (!drive.isValidFileName(newName)) { this._msg('新文件名不合法（≤31 字节，不含特殊字符）', 'error'); return false; }
      try {
        const ok = await drive.renameEntry(this.curPath, entry.name, newName, entry.isDir ? drive.TYPE_DIR : drive.TYPE_FILE);
        this._msg(ok ? `已重命名为 ${newName}` : '重命名失败：目标名被占用或源不存在', ok ? 'success' : 'error');
        if (ok) await this.refresh();
        return ok;
      } catch (e) { this._msg(e.message || '重命名失败', 'error'); return false; }
    },

    // ===== 移动（38/58，用户根相对路径）=====
    async openMoveDialog(entry) {
      this.moveSrc = entry;
      this.moveDirRel = '';
      await this._loadMoveDirs('');
    },
    closeMoveDialog() { this.moveSrc = null; this.moveDirs = []; this.moveDirRel = ''; },
    async _loadMoveDirs(rel) {
      try { this.moveDirs = await drive.listSubDirs(rel); }
      catch (e) { this._msg(e.message || '读取目录失败', 'error'); this.moveDirs = []; }
    },
    async moveBrowse(rel) {
      this.moveDirRel = rel;
      await this._loadMoveDirs(rel);
    },
    // 源条目相对用户根路径 = 当前目录相对段 + 名字。
    _relOfCurrent() {
      if (!this.curPath.startsWith(this.userRoot)) return '';
      return this.curPath.slice(this.userRoot.length).replace(/^\/+/, '');
    },
    async doMove() {
      if (!this.moveSrc) return false;
      const relDir = this._relOfCurrent();
      const srcRel = relDir ? `${relDir}/${this.moveSrc.name}` : this.moveSrc.name;
      const dstRel = this.moveDirRel;
      if (srcRel === (dstRel ? `${dstRel}/${this.moveSrc.name}` : this.moveSrc.name)) {
        this._msg('目标目录与当前位置相同（原地移动）', 'info');
        this.closeMoveDialog();
        return true;
      }
      try {
        const r = await drive.moveEntry(srcRel, dstRel, this.moveSrc.name);
        this._msg(r.ok ? `已移动${r.finalName !== this.moveSrc.name ? `（重名自动改名为 ${r.finalName}）` : ''}` : '移动失败：目标目录不存在或越权', r.ok ? 'success' : 'error');
        this.closeMoveDialog();
        if (r.ok) await this.refresh();
        return r.ok;
      } catch (e) { this._msg(e.message || '移动失败', 'error'); return false; }
    },

    // ===== 传输入口 =====
    uploadFiles(fileList) {
      const transfer = useTransferStore();
      transfer.init();
      let n = 0;
      for (const f of fileList) { transfer.enqueueUpload(f, this.curPath); n++; }
      if (n > 0) this._msg(`已加入上传队列 ${n} 个文件`, 'success');
      return n;
    },
    downloadEntry(entry) {
      if (entry.isDir) { this._msg('目录不可下载（与桌面端一致，仅普通文件）', 'error'); return false; }
      const transfer = useTransferStore();
      transfer.init();
      transfer.enqueueDownload(drive.joinPath(this.curPath, entry.name), entry.name);
      this._msg(`已加入下载队列：${entry.name}`, 'success');
      return true;
    },

    reset() {
      this.userRoot = ''; this.curPath = ''; this.entries = [];
      this.message = ''; this.moveSrc = null; this.moveDirs = []; this.moveDirRel = '';
    },
  },
});

// 便于测试：暴露默认根常量。
export { DEFAULT_ROOT } from '../protocol/commands/drive.js';
