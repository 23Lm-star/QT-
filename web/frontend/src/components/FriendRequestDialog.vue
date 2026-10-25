<script setup>
// 好友申请对话框（push 9 触发）：同意发送 10；拒绝无协议消息（与桌面端一致），仅关闭。
import { useFriendsStore } from '../stores/friends.js';

const friends = useFriendsStore();
</script>

<template>
  <div class="mask">
    <div class="dialog">
      <h3>好友申请</h3>
      <p>你同意 <b>{{ friends.incomingRequest?.requester }}</b> 的好友申请吗？</p>
      <div class="actions">
        <button class="ghost" @click="friends.declineAdd()">拒绝</button>
        <button class="primary" @click="friends.agreeAdd()">同意</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.mask { position: fixed; inset: 0; background: rgba(0,0,0,.35); display: flex; align-items: center; justify-content: center; z-index: 60; }
.dialog { width: 340px; background: #fff; border-radius: 12px; padding: 22px; text-align: center; }
h3 { margin: 0 0 10px; font-size: 16px; color: #111827; }
p { margin: 0 0 18px; font-size: 14px; color: #374151; }
.actions { display: flex; gap: 10px; justify-content: center; }
.actions button { padding: 8px 24px; border-radius: 8px; font-size: 14px; cursor: pointer; border: 1px solid transparent; }
.primary { background: #2563eb; color: #fff; }
.ghost { background: #fff; border-color: #d1d5db !important; color: #374151; }
</style>
