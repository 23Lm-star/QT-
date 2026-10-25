<script setup>
// 文件分享邀请（push 35）：接受发 36，37 回执后自动切到下一条；拒绝无协议消息（与桌面一致）。
import { useShareStore } from '../stores/share.js';

const share = useShareStore();
</script>

<template>
  <div v-if="share.activeInvite" class="mask">
    <div class="dialog">
      <h3>文件分享邀请</h3>
      <p><b>{{ share.activeInvite.sharer }}</b> 想分享文件 <b>{{ share.activeInvite.fileName }}</b> 给你</p>
      <p class="path">{{ share.activeInvite.sourceAbsPath }}</p>
      <p class="tip">接受后文件会复制到你的云盘根目录（重名自动改名）。</p>
      <p v-if="share.invites.length" class="queue">另有 {{ share.invites.length }} 条邀请排队</p>
      <div class="actions">
        <button class="ghost" :disabled="share.accepting" @click="share.declineActiveInvite()">拒绝</button>
        <button class="primary" :disabled="share.accepting" @click="share.acceptActiveInvite()">{{ share.accepting ? '接收中…' : '接受' }}</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.mask { position: fixed; inset: 0; background: rgba(0,0,0,.35); display: flex; align-items: center; justify-content: center; z-index: 60; }
.dialog { width: 420px; background: #fff; border-radius: 12px; padding: 22px; text-align: center; }
h3 { margin: 0 0 10px; font-size: 16px; color: #111827; }
p { margin: 0 0 10px; font-size: 14px; color: #374151; }
.path { font-size: 11px; color: #6b7280; font-family: Consolas, monospace; word-break: break-all; }
.tip { font-size: 12px; color: #6b7280; }
.queue { font-size: 12px; color: #b45309; }
.actions { display: flex; gap: 10px; justify-content: center; margin-top: 6px; }
.actions button { padding: 8px 24px; border-radius: 8px; font-size: 14px; cursor: pointer; border: 1px solid transparent; }
.actions button:disabled { opacity: .6; cursor: not-allowed; }
.primary { background: #2563eb; color: #fff; }
.ghost { background: #fff; border-color: #d1d5db !important; color: #374151; }
</style>
