<script setup>
// 群聊页签主布局（三栏）：左侧群列表，中间聊天区，右侧常驻小窗（群成员 + 群文件）。
// 右侧信息小窗始终贴附群窗口右缘，不覆盖中间聊天区（用户要求）。
// 生命周期（命令 60-87）在 group store + GroupMembersPanel/GroupInviteDialog 实现；
// 群文本（M4-B）填充中央聊天区，群文件（M4-C）填充右侧文件小窗。
import { onMounted } from 'vue';
import { useGroupStore } from '../stores/group.js';
import GroupMembersPanel from '../components/GroupMembersPanel.vue';
import GroupChatPanel from '../components/GroupChatPanel.vue';
import GroupFilePanel from '../components/GroupFilePanel.vue';
import GroupInviteDialog from '../components/GroupInviteDialog.vue';

const grp = useGroupStore();
onMounted(() => grp.init());

async function createGroup() {
  const name = window.prompt('请输入新群名称：');
  if (name && name.trim()) await grp.createGroup(name.trim());
}
async function leaveSelected() {
  if (grp.selectedId == null) return;
  if (window.confirm('确定退出该群？')) await grp.leaveGroup(grp.selectedId);
}
async function dissolveSelected() {
  if (grp.selectedId == null) return;
  if (window.confirm('解散群将删除群及其所有消息与文件，确定？')) await grp.dissolveGroup(grp.selectedId);
}
</script>

<template>
  <div class="group-view">
    <!-- 左：群列表 -->
    <aside class="col-list">
      <div class="list-head">
        <span>我的群（{{ grp.groups.length }}）</span>
        <div class="head-acts">
          <button class="btn primary" @click="createGroup">新建</button>
          <button class="btn" @click="grp.showInvites = true">
            邀请<span v-if="grp.inviteCount" class="badge">{{ grp.inviteCount }}</span>
          </button>
        </div>
      </div>
      <ul class="list">
        <li
          v-for="gr in grp.groups"
          :key="String(gr.groupId)"
          class="row"
          :class="{ active: grp.selectedId === gr.groupId }"
          @click="grp.selectGroup(gr.groupId)"
        >
          <span class="gname">{{ gr.name }}</span>
          <span v-if="grp.unreadFor(gr.groupId)" class="badge unread">{{ grp.unreadFor(gr.groupId) }}</span>
        </li>
        <li v-if="!grp.groups.length" class="empty">还没有群，点"新建"创建或等待邀请</li>
      </ul>
      <div v-if="grp.selectedGroup" class="list-foot">
        <button class="btn" @click="leaveSelected">退出群</button>
        <button v-if="grp.isOwner" class="btn danger" @click="dissolveSelected">解散群</button>
      </div>
    </aside>

    <!-- 中：聊天区 -->
    <section class="col-chat">
      <div v-if="grp.hint" class="hint" :class="grp.hintType">{{ grp.hint }}</div>
      <GroupChatPanel v-if="grp.selectedId != null" :group-id="grp.selectedId" :group-name="grp.selectedGroup && grp.selectedGroup.name" />
      <div v-else class="center-empty">选择左侧一个群开始聊天，或点"新建"创建群</div>
    </section>

    <!-- 右：常驻信息小窗（成员 + 文件），不遮挡聊天 -->
    <aside class="col-info">
      <GroupMembersPanel class="info-box" />
      <GroupFilePanel v-if="grp.selectedId != null" :group-id="grp.selectedId" class="info-box" />
      <div v-else class="info-box file-empty">选择群后此处显示群文件</div>
    </aside>

    <GroupInviteDialog v-if="grp.showInvites" />
  </div>
</template>

<style scoped>
.group-view { display: flex; height: 100%; min-height: 0; gap: 0; }
.col-list { width: 210px; flex: none; background: #fff; border-right: 1px solid #e5e7eb; display: flex; flex-direction: column; }
.list-head { display: flex; align-items: center; justify-content: space-between; padding: 10px 12px; border-bottom: 1px solid #eef0f3; font-size: 13px; font-weight: 600; color: #374151; }
.head-acts { display: flex; gap: 6px; }
.btn { padding: 4px 10px; font-size: 12px; border: 1px solid #d1d5db; border-radius: 5px; background: #fff; color: #374151; cursor: pointer; position: relative; }
.btn.primary { border-color: #2563eb; background: #2563eb; color: #fff; }
.btn.danger { border-color: #fca5a5; color: #b91c1c; }
.badge { margin-left: 4px; display: inline-block; min-width: 14px; height: 14px; line-height: 14px; border-radius: 7px; background: #ef4444; color: #fff; font-size: 10px; text-align: center; padding: 0 3px; }
.list { list-style: none; margin: 0; padding: 4px 0; overflow: auto; flex: 1; }
.row { display: flex; align-items: center; justify-content: space-between; padding: 9px 12px; cursor: pointer; font-size: 14px; color: #111827; border-bottom: 1px solid #f5f6f8; }
.row:hover { background: #f9fafb; }
.row.active { background: #eff6ff; color: #1d4ed8; font-weight: 600; }
.gname { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.unread { background: #ef4444; }
.empty { padding: 20px 12px; text-align: center; color: #9ca3af; font-size: 12px; }
.list-foot { display: flex; gap: 6px; padding: 8px 12px; border-top: 1px solid #eef0f3; flex: none; }

.col-chat { flex: 1; min-width: 0; display: flex; flex-direction: column; background: #f3f4f6; }
.hint { padding: 6px 14px; font-size: 12px; background: #fef9c3; color: #854d0e; border-bottom: 1px solid #fde68a; flex: none; }
.hint.error { background: #fee2e2; color: #991b1b; }
.hint.success { background: #dcfce7; color: #166534; }
.center-empty { flex: 1; display: flex; align-items: center; justify-content: center; color: #9ca3af; }

.col-info { width: 300px; flex: none; border-left: 1px solid #e5e7eb; background: #f3f4f6; display: flex; flex-direction: column; gap: 10px; padding: 10px; }
.info-box { flex: 1; min-height: 0; }
.file-empty { display: flex; align-items: center; justify-content: center; color: #9ca3af; font-size: 12px; background: #fff; border: 1px solid #e5e7eb; border-radius: 8px; }
</style>
