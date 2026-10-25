<script setup>
// 群文件面板：常驻于群窗口右侧的小窗。列群文件（99/100）、上传（101，秒传内嵌，
// 复用传输引擎 31/32）、下载（105+42/43）、删除（103/104）。群任务进度显示在面板下方，
// 带进度条 + 暂停/继续/取消（与云盘 TransferPanel 一致），并有"秒传命中"标识。
// 上传/删除完成后刷新列表（跟踪 done key 集合避免重复刷新）。
import { ref, watch, computed, onMounted } from 'vue';
import { useTransferStore } from '../stores/transfer.js';
import * as g from '../protocol/commands/group.js';

const props = defineProps({ groupId: { type: [String, Object], default: null } });
const transfer = useTransferStore();
const files = ref([]);
const loading = ref(false);
const error = ref('');
const fileInput = ref(null);
const doneSeen = ref(new Set());

// 面板小窗位置窄，用极简 fmtBytes（B/KiB/MiB/GiB 单段一位小数）
function fmtBytes(n) {
  if (n == null) return '-';
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MiB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GiB`;
}
function pct(t) {
  if (t.state === 'done') return 100;
  if (!t.totalBytes) return 0;
  return Math.min(100, Math.floor((t.doneBytes / t.totalBytes) * 100));
}
const stateText = {
  queued: '排队中', hashing: '计算摘要…', running: '传输中',
  paused: '已暂停', saving: '保存中…', done: '已完成', failed: '失败', cancelled: '已取消',
};
function stateLabel(t) {
  if (t.state === 'done' && t.fastHit) return '秒传命中·已完成';
  if (t.state === 'done' && t.kind === 'upload' && t.resumeFrom) return '续传完成';
  return stateText[t.state] || t.state;
}
const canPause = (t) => ['running', 'hashing', 'queued'].includes(t.state);
const canResume = (t) => ['paused', 'failed'].includes(t.state);

const groupTasks = computed(() =>
  transfer.tasks.filter((t) => String(t.groupId) === String(props.groupId))
);

async function loadFiles() {
  if (props.groupId == null) { files.value = []; return; }
  loading.value = true; error.value = '';
  try {
    const r = await g.listGroupFiles(props.groupId, 0, 200);
    if (r.code !== 0) error.value = g.describeGroupCode(r.code) || `列文件失败 code=${r.code}`;
    else files.value = r.files;
  } catch (e) { error.value = e.message || '获取群文件失败'; }
  finally { loading.value = false; }
}

function pickUpload() { if (fileInput.value) fileInput.value.click(); }
function onFilesChosen(e) {
  const chosen = Array.from(e.target.files || []);
  for (const file of chosen) transfer.enqueueGroupUpload(props.groupId, file, file.name);
  e.target.value = '';
}

function download(f) {
  transfer.enqueueGroupDownload(props.groupId, f.gfId, f.name);
}

async function remove(f) {
  if (!window.confirm(`删除群文件"${f.name}"？（仅移除群内引用，物理内容按规则回收）`)) return;
  try {
    const r = await g.deleteGroupFile(props.groupId, f.gfId);
    if (r.code === 0) await loadFiles();
    else error.value = g.describeGroupCode(r.code) || `删除失败 code=${r.code}`;
  } catch (e) { error.value = e.message || '删除失败'; }
}

// 群任务里出现"done/failed/cancelled"新落定时才刷新列表（避免每次进度 tick 都触发网络）。
watch(groupTasks, (list) => {
  for (const t of list) {
    if (['done', 'failed', 'cancelled'].includes(t.state) && !doneSeen.value.has(t.key)) {
      doneSeen.value.add(t.key);
      if (t.kind === 'upload' && t.state === 'done') loadFiles();
    }
  }
});

watch(() => props.groupId, (gid) => { doneSeen.value = new Set(); loadFiles(); });
onMounted(() => loadFiles());
</script>

<template>
  <div class="files">
    <div class="f-head">
      <span class="f-title">群文件（{{ files.length }}）</span>
      <button class="f-up" @click="pickUpload">上传</button>
      <input ref="fileInput" type="file" multiple hidden @change="onFilesChosen" />
    </div>
    <div v-if="error" class="f-err">{{ error }}</div>
    <ul class="f-list">
      <li v-for="f in files" :key="String(f.gfId)" class="f-item">
        <div class="f-main">
          <span class="f-name" :title="f.name">{{ f.name }}</span>
          <span class="f-sub">{{ f.uploader }} · {{ fmtBytes(Number(f.size)) }}</span>
        </div>
        <div class="f-acts">
          <button class="mini" @click="download(f)">下载</button>
          <button class="mini danger" @click="remove(f)">删除</button>
        </div>
      </li>
      <li v-if="!files.length && !loading" class="f-empty">暂无群文件</li>
    </ul>

    <div v-if="groupTasks.length" class="g-prog">
      <div class="gp-head">
        <span>本群任务（{{ groupTasks.length }}）</span>
        <button class="mini ghost" @click="transfer.clearFinished()">清除已完成</button>
      </div>
      <div v-for="t in groupTasks" :key="t.key" class="gp-row" :class="{ failed: t.state === 'failed' }">
        <div class="gp-line1">
          <span class="gp-name" :title="t.name">{{ t.name }}</span>
          <span class="gp-dir">{{ t.kind === 'upload' ? '上传' : '下载' }}</span>
        </div>
        <div class="gp-bar"><div class="gp-fill" :style="{ width: pct(t) + '%' }" /></div>
        <div class="gp-line2">
          <span class="gp-nums">{{ pct(t) }}% · {{ fmtBytes(t.doneBytes) }}/{{ fmtBytes(t.totalBytes) }}</span>
          <span class="gp-state" :class="t.state">{{ stateLabel(t) }}</span>
        </div>
        <div v-if="t.failReason" class="gp-reason" :title="t.failReason">{{ t.failReason }}</div>
        <div class="gp-ops">
          <button v-if="canPause(t)" class="mini" @click="transfer.pause(t.key)">暂停</button>
          <button v-if="canResume(t)" class="mini primary" @click="transfer.resume(t.key)">继续</button>
          <button v-if="t.state !== 'done' && t.state !== 'cancelled'" class="mini danger" @click="transfer.cancel(t.key)">取消</button>
          <button v-else class="mini ghost" @click="transfer.cancel(t.key)">移除</button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.files { background: #fff; border: 1px solid #e5e7eb; border-radius: 8px; display: flex; flex-direction: column; min-height: 0; }
.f-head { display: flex; align-items: center; justify-content: space-between; padding: 8px 10px; border-bottom: 1px solid #eef0f3; flex: none; }
.f-title { font-size: 13px; font-weight: 600; color: #111827; }
.f-up { padding: 3px 10px; font-size: 12px; border: none; border-radius: 5px; background: #2563eb; color: #fff; cursor: pointer; }
.f-err { padding: 6px 10px; font-size: 12px; color: #b91c1c; background: #fef2f2; flex: none; }
.f-list { list-style: none; margin: 0; padding: 4px 0; overflow: auto; flex: 1; min-height: 80px; }
.f-item { padding: 6px 10px; border-bottom: 1px solid #f5f6f8; }
.f-main { display: flex; flex-direction: column; }
.f-name { font-size: 13px; color: #111827; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.f-sub { font-size: 11px; color: #9ca3af; margin-top: 1px; }
.f-acts { display: flex; gap: 6px; margin-top: 4px; }
.mini { padding: 2px 8px; font-size: 11px; border: 1px solid #d1d5db; border-radius: 4px; background: #fff; color: #374151; cursor: pointer; }
.mini:hover { background: #f9fafb; }
.mini.danger { border-color: #fca5a5; color: #b91c1c; }
.mini.primary { border-color: #2563eb; background: #2563eb; color: #fff; }
.mini.ghost { color: #6b7280; border-color: #e5e7eb; }
.f-empty { padding: 16px; text-align: center; color: #9ca3af; font-size: 12px; }

.g-prog { border-top: 1px solid #eef0f3; padding: 8px 10px; flex: none; max-height: 45%; overflow: auto; background: #fbfcfd; }
.gp-head { display: flex; align-items: center; justify-content: space-between; font-size: 11px; color: #6b7280; margin-bottom: 4px; }
.gp-row { padding: 6px 0; border-top: 1px solid #f1f3f6; }
.gp-row.failed { background: #fff7f7; }
.gp-line1 { display: flex; justify-content: space-between; align-items: baseline; gap: 6px; }
.gp-name { font-size: 12px; color: #111827; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; flex: 1; }
.gp-dir { font-size: 10px; color: #6b7280; flex: none; }
.gp-bar { height: 6px; background: #e5e7eb; border-radius: 3px; overflow: hidden; margin-top: 4px; }
.gp-fill { height: 100%; background: #2563eb; transition: width .18s; }
.gp-line2 { display: flex; justify-content: space-between; align-items: center; margin-top: 2px; }
.gp-nums { font-size: 10px; color: #6b7280; }
.gp-state { font-size: 11px; padding: 1px 6px; border-radius: 6px; background: #f3f4f6; color: #374151; }
.gp-state.running { background: #dbeafe; color: #1d4ed8; }
.gp-state.hashing { background: #fef3c7; color: #92400e; }
.gp-state.done { background: #dcfce7; color: #15803d; }
.gp-state.failed { background: #fee2e2; color: #b91c1c; }
.gp-state.paused { background: #fef3c7; color: #b45309; }
.gp-reason { font-size: 10px; color: #b91c1c; margin-top: 2px; max-width: 260px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.gp-ops { display: flex; gap: 4px; margin-top: 4px; }
</style>
