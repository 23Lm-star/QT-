<script setup>
// 云盘文件选择器（分享用）：23 列目录，目录逐级进入，仅普通文件可选中。
import { ref, watch } from 'vue';
import { listDirectory, joinPath } from '../protocol/commands/drive.js';

const props = defineProps({
  open: { type: Boolean, default: false },
  startPath: { type: String, default: '' },
  title: { type: String, default: '选择云盘文件' },
});
const emit = defineEmits(['close', 'select']);

const curPath = ref('');
const entries = ref([]);
const loading = ref(false);
const error = ref('');

async function load() {
  if (!curPath.value) return;
  loading.value = true;
  error.value = '';
  try {
    entries.value = await listDirectory(curPath.value);
  } catch (e) {
    error.value = e.message || '读取目录失败';
    entries.value = [];
  } finally {
    loading.value = false;
  }
}

watch(() => props.open, async (v) => {
  if (!v) return;
  curPath.value = props.startPath;
  await load();
});

async function enter(name) {
  curPath.value = joinPath(curPath.value, name);
  await load();
}
async function up() {
  if (curPath.value === props.startPath) return;
  const idx = curPath.value.lastIndexOf('/');
  if (idx <= 0) return;
  curPath.value = curPath.value.slice(0, idx);
  await load();
}
function pick(entry) {
  if (entry.isDir) { enter(entry.name); return; }
  emit('select', { name: entry.name, absPath: joinPath(curPath.value, entry.name) });
}
</script>

<template>
  <div v-if="open" class="mask" @click.self="emit('close')">
    <div class="dialog">
      <h3>{{ title }}</h3>
      <div class="nav">
        <button class="mini" :disabled="curPath === startPath" @click="up">上一级</button>
        <span class="path" :title="curPath">{{ curPath }}</span>
        <button class="mini" @click="load">刷新</button>
      </div>
      <div v-if="error" class="err">{{ error }}</div>
      <ul class="list">
        <li v-if="loading" class="empty">加载中…</li>
        <li v-else-if="entries.length === 0" class="empty">空目录</li>
        <li v-for="e in entries" :key="e.name" @click="pick(e)">
          <span class="tag" :class="e.isDir ? 'dir' : 'file'">{{ e.isDir ? '目录' : '文件' }}</span>
          <span class="name">{{ e.name }}</span>
          <span class="hint">{{ e.isDir ? '进入' : '选择' }}</span>
        </li>
      </ul>
      <div class="actions">
        <button class="ghost" @click="emit('close')">取消</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.mask { position: fixed; inset: 0; background: rgba(0,0,0,.35); display: flex; align-items: center; justify-content: center; z-index: 55; }
.dialog { width: 480px; max-height: 72vh; background: #fff; border-radius: 12px; padding: 18px; display: flex; flex-direction: column; gap: 10px; }
h3 { margin: 0; font-size: 15px; color: #111827; }
.nav { display: flex; align-items: center; gap: 8px; }
.path { flex: 1; font-size: 12px; color: #6b7280; font-family: Consolas, monospace; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.list { list-style: none; margin: 0; padding: 4px; border: 1px solid #e5e7eb; border-radius: 8px; overflow-y: auto; flex: 1; min-height: 160px; }
.list li { display: flex; align-items: center; gap: 8px; padding: 7px 10px; border-radius: 6px; cursor: pointer; font-size: 13px; }
.list li:hover { background: #f3f4f6; }
.list li.empty { color: #9ca3af; cursor: default; justify-content: center; }
.tag { font-size: 11px; padding: 1px 8px; border-radius: 8px; flex: none; }
.tag.dir { background: #fef3c7; color: #b45309; }
.tag.file { background: #dbeafe; color: #1d4ed8; }
.name { word-break: break-all; }
.hint { margin-left: auto; font-size: 11px; color: #9ca3af; flex: none; }
.err { font-size: 12px; color: #b91c1c; }
.actions { display: flex; justify-content: flex-end; }
.actions button { padding: 7px 18px; border-radius: 8px; font-size: 13px; cursor: pointer; border: 1px solid #d1d5db; background: #fff; color: #374151; }
.mini { padding: 3px 10px; font-size: 12px; border: 1px solid #d1d5db; border-radius: 6px; background: #fff; cursor: pointer; color: #374151; }
.mini:disabled { color: #9ca3af; cursor: not-allowed; }
</style>
