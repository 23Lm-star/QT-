<script setup>
// 群成员面板：常驻于群窗口右侧的小窗，展示选中群成员与角色，并按我的角色提供
// 邀请/设角色/移除/转让操作。数据来自 group store 的 members（命令 80/81）。
import { computed } from 'vue';
import { useGroupStore } from '../stores/group.js';

const grp = useGroupStore();

const canManage = computed(() => grp.isAdmin);
const isOwner = computed(() => grp.isOwner);

async function invite() {
  const name = window.prompt('请输入要邀请的用户名：');
  if (name && name.trim()) await grp.inviteMember(name.trim());
}
async function setRole(m, role) {
  if (window.confirm(`将 ${m.name} 设为${role === 'admin' ? '管理员' : '成员'}？`)) {
    await grp.setMemberRole(m.name, role);
  }
}
async function remove(m) {
  if (window.confirm(`将 ${m.name} 移出群聊？`)) await grp.removeMember(m.name);
}
async function transfer(m) {
  if (window.confirm(`将群主转让给 ${m.name}？你将变为管理员。`)) await grp.transferGroupOwner(m.name);
}
</script>

<template>
  <div class="members">
    <div class="m-head">
      <span class="m-title">群成员（{{ grp.members.length }}）</span>
      <button v-if="canManage" class="m-invite" @click="invite">邀请</button>
    </div>
    <ul class="m-list">
      <li v-for="m in grp.members" :key="m.name" class="m-item">
        <div class="m-info">
          <span class="m-name">{{ m.name }}</span>
          <span class="m-role" :class="m.role">{{ m.role }}</span>
          <span class="m-status" :class="m.status">{{ m.status === 'active' ? '在群' : m.status }}</span>
        </div>
        <div v-if="canManage && m.role !== 'owner'" class="m-acts">
          <button v-if="m.role !== 'admin'" class="mini" @click="setRole(m, 'admin')">设为管理员</button>
          <button v-else class="mini" @click="setRole(m, 'member')">取消管理员</button>
          <button v-if="isOwner" class="mini" @click="transfer(m)">转让</button>
          <button class="mini danger" @click="remove(m)">移除</button>
        </div>
      </li>
      <li v-if="!grp.members.length" class="m-empty">未选择群或暂无成员</li>
    </ul>
  </div>
</template>

<style scoped>
.members { background: #fff; border: 1px solid #e5e7eb; border-radius: 8px; display: flex; flex-direction: column; min-height: 0; }
.m-head { display: flex; align-items: center; justify-content: space-between; padding: 8px 10px; border-bottom: 1px solid #eef0f3; flex: none; }
.m-title { font-size: 13px; font-weight: 600; color: #111827; }
.m-invite { padding: 3px 10px; font-size: 12px; border: none; border-radius: 5px; background: #2563eb; color: #fff; cursor: pointer; }
.m-list { list-style: none; margin: 0; padding: 4px 0; overflow: auto; flex: 1; min-height: 0; }
.m-item { padding: 6px 10px; border-bottom: 1px solid #f5f6f8; }
.m-info { display: flex; align-items: center; gap: 6px; }
.m-name { font-size: 13px; color: #111827; }
.m-role { font-size: 10px; padding: 1px 6px; border-radius: 4px; background: #eef2ff; color: #4338ca; }
.m-role.owner { background: #fef3c7; color: #92400e; }
.m-status { font-size: 10px; color: #6b7280; }
.m-acts { display: flex; gap: 4px; margin-top: 4px; flex-wrap: wrap; }
.mini { padding: 2px 6px; font-size: 11px; border: 1px solid #d1d5db; border-radius: 4px; background: #fff; color: #374151; cursor: pointer; }
.mini.danger { border-color: #fca5a5; color: #b91c1c; }
.m-empty { padding: 16px; text-align: center; color: #9ca3af; font-size: 12px; }
</style>
