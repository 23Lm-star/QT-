<script setup>
// 云盘页：目录浏览（23）+ 面包屑 + 新建(19)/删除(25)/重命名(27)/移动(38+58)/跳转(21)
// + 上传（File 多选 → transfer 队列）+ 下载（40 链）+ 传输面板。
import { ref, onMounted } from 'vue';
import { useAuthStore } from '../stores/auth.js';
import { useDriveStore } from '../stores/drive.js';
import { useTransferStore } from '../stores/transfer.js';
import TransferPanel from '../components/TransferPanel.vue';

const auth = useAuthStore();
const drv = useDriveStore();
const transfer = useTransferStore();

const selected = ref(null);       // 当前选中条目（单选，与桌面端一致）
const pathInput = ref('');
const showPanel = ref(true);
const fileInput = ref(null);

onMounted(async () => {
  transfer.init();
  pathInput.value = drv.curPath || '';
  if (!drv.userRoot) await drv.enter(auth.loginName);
  else await drv.refresh();
  pathInput.value = drv.curPath;
});

function select(entry) { selected.value = entry; }
async function dblclick(entry) {
  if (entry.isDir) {
    await drv.enterDir(entry.name);
    selected.value = null;
    pathInput.value = drv.curPath;
  }
}
async function goCrumb(abs) {
  await drv.gotoCrumb(abs);
  selected.value = null;
  pathInput.value = drv.curPath;
}
async function doBack() {
  await drv.back();
  selected.value = null;
  pathInput.value = drv.curPath;
}
async function doChdir() {
  await drv.chdirTo(pathInput.value);
  pathInput.value = drv.curPath;
  selected.value = null;
}
async function doMkdir() {
  const name = window.prompt('新建文件夹名（≤31 字节，不含 <>:"/\\|?* ）');
  if (name == null) return;
  await drv.mkdir(name);
}
async function doRename() {
  if (!selected.value) { drv._msg('请先选择要重命名的文件/目录', 'error'); return; }
  const name = window.prompt('新名称', selected.value.name);
  if (name == null) return;
  await drv.rename(selected.value, name);
  selected.value = null;
}
async function doDelete() {
  if (!selected.value) { drv._msg('请先选择要删除的文件/目录', 'error'); return; }
  const e = selected.value;
  const tip = e.isDir
    ? `确定删除目录 ${e.name} 吗？\n目录子树将全部删除，相关分享链接联动失效，不可恢复。`
    : `确定删除文件 ${e.name} 吗？\n引用计数归零后内容进入回收，相关分享链接联动失效。`;
  if (!window.confirm(tip)) return;
  await drv.remove(e);
  selected.value = null;
}
async function doMove() {
  if (!selected.value) { drv._msg('请先选择要移动的文件/目录', 'error'); return; }
  await drv.openMoveDialog(selected.value);
}
async function moveUp() {
  if (!drv.moveDirRel) return;
  const idx = drv.moveDirRel.lastIndexOf('/');
  await drv.moveBrowse(idx < 0 ? '' : drv.moveDirRel.slice(0, idx));
}
async function moveInto(name) {
  await drv.moveBrowse(drv.moveDirRel ? `${drv.moveDirRel}/${name}` : name);
}
async function moveConfirm() {
  await drv.doMove();
  selected.value = null;
}
function pickFiles() { fileInput.value && fileInput.value.click(); }
function onFilesChosen(ev) {
  const files = Array.from(ev.target.files || []);
  if (files.length) drv.uploadFiles(files);
  ev.target.value = ''; // 允许重复选同一文件
}
function doDownload() {
  if (!selected.value) { drv._msg('请先选择要下载的文件', 'error'); return; }
  if (selected.value.isDir) { drv._msg('目录不可下载（与桌面端一致）', 'error'); return; }
  drv.downloadEntry(selected.value);
}
</script>

<template>
  <div class="drive">
    <div class="toolbar">
      <button @click="doBack" :disabled="drv.curPath === drv.userRoot">返回上级</button>
      <input class="path" v-model="pathInput" @keydown.enter="doChdir" title="服务端路径字面量（须与服务端文件根一致）" />
      <button @click="doChdir">跳转</button>
      <button @click="drv.refresh()">刷新</button>
      <button @click="doMkdir">新建文件夹</button>
      <button class="primary" @click="pickFiles">上传文件</button>
      <button @click="doDownload">下载</button>
      <button @click="doRename">重命名</button>
      <button @click="doMove">移动</button>
      <button class="danger" @click="doDelete">删除</button>
      <button class="toggle" @click="showPanel = !showPanel">{{ showPanel ? '收起任务' : `传输任务(${transfer.tasks.length})` }}</button>
      <input ref="fileInput" type="file" multiple style="display:none" @change="onFilesChosen" />
    </div>

    <div class="crumbs">
      <template v-for="(c, i) in drv.crumbs" :key="c.abs">
        <span v-if="i > 0" class="sep">/</span>
        <a :class="{ current: i === drv.crumbs.length - 1 }" @click="goCrumb(c.abs)">{{ c.label }}</a>
      </template>
    </div>

    <div v-if="drv.message" class="msg" :class="drv.messageType">{{ drv.message }}</div>

    <div class="list-wrap">
      <table class="entries">
        <thead><tr><th class="w-type">类型</th><th>名称</th><th class="w-ops">快捷操作</th></tr></thead>
        <tbody>
          <tr v-if="drv.loading"><td colspan="3" class="empty">加载中…</td></tr>
          <tr v-else-if="drv.entries.length === 0"><td colspan="3" class="empty">空目录（可上传文件或新建文件夹）</td></tr>
          <tr v-for="e in drv.entries" :key="e.name"
              :class="{ selected: selected && selected.name === e.name }"
              @click="select(e)" @dblclick="dblclick(e)">
            <td class="w-type"><span class="tag" :class="e.isDir ? 'dir' : 'file'">{{ e.isDir ? '目录' : '文件' }}</span></td>
            <td class="ename" :title="e.isDir ? '双击进入' : ''">{{ e.name }}</td>
            <td class="w-ops">
              <button v-if="!e.isDir" class="mini" @click.stop="drv.downloadEntry(e)">下载</button>
              <button class="mini" @click.stop="selected = e; doMove()">移动</button>
              <button class="mini danger" @click.stop="selected = e; doDelete()">删除</button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <div v-if="showPanel" class="panel-wrap">
      <TransferPanel />
    </div>

    <!-- 移动对话框：58 listdir 浏览用户根相对目录 -->
    <div v-if="drv.moveSrc" class="mask" @click.self="drv.closeMoveDialog()">
      <div class="dialog">
        <h3>移动「{{ drv.moveSrc.name }}」到…</h3>
        <div class="mv-path">相对目录：/{{ drv.moveDirRel }}</div>
        <ul class="mv-list">
          <li v-if="drv.moveDirRel" @click="moveUp">..（返回上级）</li>
          <li v-if="drv.moveDirs.length === 0" class="empty">此目录下没有子目录</li>
          <li v-for="d in drv.moveDirs" :key="d" @click="moveInto(d)">{{ d }}/</li>
        </ul>
        <div class="mv-actions">
          <button class="ghost" @click="drv.closeMoveDialog()">取消</button>
          <button class="primary" @click="moveConfirm">移动到这里</button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.drive { display: flex; flex-direction: column; height: 100%; min-height: 0; padding: 12px 16px; gap: 8px; box-sizing: border-box; }
.toolbar { display: flex; gap: 6px; flex-wrap: wrap; align-items: center; }
.toolbar button { padding: 6px 12px; font-size: 13px; border: 1px solid #d1d5db; border-radius: 6px; background: #fff; cursor: pointer; color: #374151; }
.toolbar button:hover:not(:disabled) { background: #f9fafb; }
.toolbar button:disabled { color: #9ca3af; cursor: not-allowed; }
.toolbar .primary { background: #2563eb; border-color: #2563eb; color: #fff; }
.toolbar .danger { color: #b91c1c; border-color: #fecaca; }
.toolbar .toggle { margin-left: auto; }
.path { flex: 1; min-width: 220px; padding: 6px 10px; border: 1px solid #d1d5db; border-radius: 6px; font-size: 13px; font-family: Consolas, monospace; }
.crumbs { font-size: 13px; color: #4b5563; display: flex; flex-wrap: wrap; gap: 2px; align-items: center; }
.crumbs a { color: #2563eb; cursor: pointer; }
.crumbs a.current { color: #111827; font-weight: 600; cursor: default; }
.crumbs .sep { color: #9ca3af; margin: 0 2px; }
.msg { font-size: 13px; padding: 7px 10px; border-radius: 6px; }
.msg.info { background: #eff6ff; color: #1d4ed8; }
.msg.success { background: #ecfdf5; color: #047857; }
.msg.error { background: #fef2f2; color: #b91c1c; }
.list-wrap { flex: 1; min-height: 120px; overflow-y: auto; background: #fff; border: 1px solid #e5e7eb; border-radius: 10px; }
.entries { width: 100%; border-collapse: collapse; font-size: 13px; }
.entries th, .entries td { padding: 7px 12px; border-bottom: 1px solid #f3f4f6; text-align: left; }
.entries th { color: #6b7280; font-weight: 500; position: sticky; top: 0; background: #fcfcfd; }
.entries tbody tr { cursor: pointer; }
.entries tbody tr:hover { background: #f9fafb; }
.entries tbody tr.selected { background: #eff6ff; }
.w-type { width: 64px; }
.w-ops { width: 200px; white-space: nowrap; }
.tag { font-size: 11px; padding: 1px 8px; border-radius: 8px; }
.tag.dir { background: #fef3c7; color: #b45309; }
.tag.file { background: #dbeafe; color: #1d4ed8; }
.ename { word-break: break-all; }
.empty { text-align: center; color: #9ca3af; padding: 18px !important; }
.mini { padding: 2px 10px; font-size: 12px; border: 1px solid #d1d5db; border-radius: 6px; background: #fff; cursor: pointer; color: #374151; margin-right: 4px; }
.mini.danger { color: #b91c1c; border-color: #fecaca; }
.panel-wrap { flex: none; max-height: 240px; overflow-y: auto; }
.mask { position: fixed; inset: 0; background: rgba(0,0,0,.35); display: flex; align-items: center; justify-content: center; z-index: 50; }
.dialog { width: 420px; max-height: 70vh; background: #fff; border-radius: 12px; padding: 18px; display: flex; flex-direction: column; }
.dialog h3 { margin: 0 0 8px; font-size: 15px; }
.mv-path { font-size: 12px; color: #6b7280; font-family: Consolas, monospace; margin-bottom: 8px; }
.mv-list { list-style: none; margin: 0 0 12px; padding: 4px; border: 1px solid #e5e7eb; border-radius: 8px; overflow-y: auto; flex: 1; min-height: 120px; }
.mv-list li { padding: 7px 10px; border-radius: 6px; cursor: pointer; font-size: 13px; }
.mv-list li:hover { background: #f3f4f6; }
.mv-list li.empty { color: #9ca3af; cursor: default; text-align: center; }
.mv-actions { display: flex; gap: 10px; justify-content: flex-end; }
.mv-actions button { padding: 7px 18px; border-radius: 8px; font-size: 13px; cursor: pointer; border: 1px solid transparent; }
.mv-actions .primary { background: #2563eb; color: #fff; }
.mv-actions .ghost { background: #fff; border-color: #d1d5db; color: #374151; }
</style>
