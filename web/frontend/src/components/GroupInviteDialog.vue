<script setup>
// 我的群邀请弹窗：列出他人对我的入群邀请（命令 82/83），接受(64)/拒绝(66)。
import { useGroupStore } from '../stores/group.js';
const grp = useGroupStore();
</script>

<template>
  <div class="mask" @click.self="grp.showInvites = false">
    <div class="dlg">
      <div class="dlg-head">
        <span>我的群邀请</span>
        <button class="close" @click="grp.showInvites = false">×</button>
      </div>
      <ul class="list">
        <li v-for="it in grp.invitations" :key="String(it.invitationId)" class="row">
          <div class="meta">
            <div class="gname">{{ it.groupName }}</div>
            <div class="sub">来自 {{ it.inviter }} · {{ it.status }}</div>
          </div>
          <div class="acts">
            <button class="ok" @click="grp.acceptInvitation(it.invitationId)">接受</button>
            <button class="no" @click="grp.rejectInvitation(it.invitationId)">拒绝</button>
          </div>
        </li>
        <li v-if="!grp.invitations.length" class="empty">暂无待处理邀请</li>
      </ul>
    </div>
  </div>
</template>

<style scoped>
.mask { position: fixed; inset: 0; background: rgba(0,0,0,.35); display: flex; align-items: center; justify-content: center; z-index: 50; }
.dlg { width: 460px; max-height: 70vh; background: #fff; border-radius: 10px; display: flex; flex-direction: column; overflow: hidden; }
.dlg-head { display: flex; align-items: center; justify-content: space-between; padding: 12px 16px; border-bottom: 1px solid #eef0f3; font-weight: 600; }
.close { border: none; background: none; font-size: 20px; cursor: pointer; color: #6b7280; }
.list { list-style: none; margin: 0; padding: 8px 0; overflow: auto; }
.row { display: flex; align-items: center; justify-content: space-between; padding: 10px 16px; border-bottom: 1px solid #f5f6f8; }
.gname { font-size: 14px; color: #111827; }
.sub { font-size: 12px; color: #6b7280; margin-top: 2px; }
.acts { display: flex; gap: 8px; }
.ok { padding: 5px 14px; border: none; border-radius: 5px; background: #16a34a; color: #fff; cursor: pointer; font-size: 13px; }
.no { padding: 5px 14px; border: 1px solid #d1d5db; border-radius: 5px; background: #fff; color: #374151; cursor: pointer; font-size: 13px; }
.empty { padding: 24px; text-align: center; color: #9ca3af; font-size: 13px; }
</style>
