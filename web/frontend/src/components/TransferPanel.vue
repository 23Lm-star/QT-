<script setup>
// 传输任务面板：进度/状态/失败原因 + 暂停/继续/取消/清除已完成。
import { useTransferStore } from '../stores/transfer.js';

const transfer = useTransferStore();

const stateText = {
  queued: '排队中', hashing: '计算摘要…', running: '传输中',
  paused: '已暂停', saving: '保存中…', done: '已完成', failed: '失败', cancelled: '已取消',
};
function pct(t) {
  if (t.state === 'done') return 100;
  if (!t.totalBytes) return 0;
  return Math.min(100, Math.floor((t.doneBytes / t.totalBytes) * 100));
}
function fmtBytes(n) {
  if (n == null) return '-';
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MiB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GiB`;
}
</script>

<template>
  <div class="panel">
    <div class="panel-head">
      <span>传输任务（上传/下载各 3 并发；断点续传仅本页面会话内有效）；下载完成前不落盘，完成后保存到浏览器下载目录（可在浏览器设置开启"每次下载前询问位置"）</span>
      <button class="mini" @click="transfer.clearFinished()">清除已完成</button>
    </div>
    <div v-if="transfer.tasks.length === 0" class="empty">暂无传输任务</div>
    <table v-else>
      <thead>
        <tr><th>文件</th><th>方向</th><th class="w-prog">进度</th><th>状态</th><th>操作</th></tr>
      </thead>
      <tbody>
        <tr v-for="t in transfer.tasks" :key="t.key" :class="{ failed: t.state === 'failed' }">
          <td class="name" :title="t.finalName ? `服务端最终名：${t.finalName}` : t.name">{{ t.name }}</td>
          <td>{{ t.kind === 'upload' ? '上传' : '下载' }}</td>
          <td class="w-prog">
            <div class="bar"><div class="fill" :style="{ width: pct(t) + '%' }" /></div>
            <span class="nums">{{ pct(t) }}% · {{ fmtBytes(t.doneBytes) }}/{{ fmtBytes(t.totalBytes) }}</span>
          </td>
          <td>
            <span class="state" :class="t.state">{{ stateText[t.state] || t.state }}</span>
            <div v-if="t.state === 'hashing' && t.hashProgress" class="hash">
              摘要 {{ Math.floor((t.hashProgress / (t.totalBytes || 1)) * 100) }}%
            </div>
            <div v-if="t.failReason" class="reason" :title="t.failReason">{{ t.failReason }}</div>
          </td>
          <td class="ops">
            <button v-if="['running', 'hashing', 'queued'].includes(t.state)" class="mini" @click="transfer.pause(t.key)">暂停</button>
            <button v-if="['paused', 'failed'].includes(t.state)" class="mini" @click="transfer.resume(t.key)">继续</button>
            <button v-if="!['done', 'cancelled'].includes(t.state)" class="mini danger" @click="transfer.cancel(t.key)">取消</button>
            <button v-else class="mini" @click="transfer.cancel(t.key)">移除</button>
          </td>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<style scoped>
.panel { background: #fff; border: 1px solid #e5e7eb; border-radius: 10px; overflow: hidden; }
.panel-head { display: flex; align-items: center; justify-content: space-between; padding: 8px 12px; background: #f9fafb; border-bottom: 1px solid #e5e7eb; font-size: 13px; font-weight: 600; color: #374151; }
.empty { padding: 16px; text-align: center; color: #9ca3af; font-size: 13px; }
table { width: 100%; border-collapse: collapse; font-size: 13px; }
th, td { padding: 7px 10px; border-bottom: 1px solid #f3f4f6; text-align: left; vertical-align: top; }
th { color: #6b7280; font-weight: 500; background: #fcfcfd; }
tr.failed td { background: #fff7f7; }
.name { max-width: 220px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.w-prog { width: 220px; }
.bar { height: 8px; background: #e5e7eb; border-radius: 4px; overflow: hidden; }
.fill { height: 100%; background: #2563eb; transition: width .2s; }
.nums { font-size: 11px; color: #6b7280; }
.state { font-size: 12px; padding: 1px 8px; border-radius: 8px; background: #f3f4f6; color: #374151; }
.state.running { background: #dbeafe; color: #1d4ed8; }
.state.done { background: #dcfce7; color: #15803d; }
.state.failed { background: #fee2e2; color: #b91c1c; }
.state.paused { background: #fef3c7; color: #b45309; }
.hash { font-size: 11px; color: #6b7280; margin-top: 2px; }
.reason { font-size: 11px; color: #b91c1c; margin-top: 2px; max-width: 200px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.ops { white-space: nowrap; }
.mini { padding: 2px 10px; font-size: 12px; border: 1px solid #d1d5db; border-radius: 6px; background: #fff; cursor: pointer; color: #374151; margin-right: 4px; }
.mini:hover { background: #f9fafb; }
.mini.danger { color: #b91c1c; border-color: #fecaca; }
</style>
