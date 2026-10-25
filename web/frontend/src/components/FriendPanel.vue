<script setup>
// 好友面板：左侧好友列表 + 工具条 + 提示条，右侧聊天区。
import { ref } from 'vue';
import { useFriendsStore } from '../stores/friends.js';
import { useChatStore } from '../stores/chat.js';
import ChatPanel from './ChatPanel.vue';
import OnlineUserDialog from './OnlineUserDialog.vue';

const friends = useFriendsStore();
const chat = useChatStore();
const showOnline = ref(false);

function onFriendClick(name) { chat.openChat(name); }

function onDeleteFriend() {
  const target = chat.activePeer;
  if (!target) return;
  if (window.confirm(`确定删除好友 ${target} 吗？\n服务端将同时清除双方私聊记录，且不可恢复。`)) {
    friends.removeFriend(target);
  }
}
</script>

<template>
  <div class="friend-panel">
    <aside class="side">
      <div class="toolbar">
        <button @click="friends.loadFriends()">刷新好友</button>
        <button @click="showOnline = true; friends.loadOnlineUsers()">在线用户</button>
        <button class="danger" :disabled="!chat.activePeer" @click="onDeleteFriend">删除好友</button>
      </div>
      <div v-if="friends.hint" class="hint" :class="friends.hintType">{{ friends.hint }}</div>
      <ul class="list">
        <li v-if="friends.friends.length === 0" class="empty">暂无好友，点击"在线用户"添加</li>
        <li v-for="name in friends.friends" :key="name"
            class="item" :class="{ active: chat.activePeer === name }" @click="onFriendClick(name)">
          <span class="avatar">{{ name.slice(0, 1).toUpperCase() }}</span>
          <span class="name">{{ name }}</span>
          <span v-if="chat.unread[name]" class="badge">{{ chat.unread[name] }}</span>
        </li>
      </ul>
    </aside>
    <section class="chat-area">
      <ChatPanel v-if="chat.activePeer" :peer="chat.activePeer" />
      <div v-else class="chat-empty">选择左侧好友开始聊天</div>
    </section>

    <OnlineUserDialog v-if="showOnline" @close="showOnline = false" />
  </div>
</template>

<style scoped>
.friend-panel { display: flex; height: 100%; min-height: 0; }
.side { width: 280px; flex: none; background: #fff; border-right: 1px solid #e5e7eb; display: flex; flex-direction: column; }
.toolbar { display: flex; gap: 6px; padding: 10px; border-bottom: 1px solid #f3f4f6; }
.toolbar button { flex: 1; padding: 6px 4px; font-size: 12px; border: 1px solid #d1d5db; border-radius: 6px; background: #fff; cursor: pointer; color: #374151; }
.toolbar button:hover:not(:disabled) { background: #f9fafb; }
.toolbar button:disabled { color: #9ca3af; cursor: not-allowed; }
.toolbar .danger { color: #b91c1c; border-color: #fecaca; }
.hint { padding: 8px 10px; font-size: 12px; border-bottom: 1px solid #f3f4f6; }
.hint.info { background: #eff6ff; color: #1d4ed8; }
.hint.success { background: #ecfdf5; color: #047857; }
.hint.error { background: #fef2f2; color: #b91c1c; }
.list { flex: 1; overflow-y: auto; margin: 0; padding: 6px; list-style: none; }
.list .empty { padding: 16px 10px; font-size: 12px; color: #9ca3af; text-align: center; }
.item { display: flex; align-items: center; gap: 10px; padding: 9px 10px; border-radius: 8px; cursor: pointer; }
.item:hover { background: #f9fafb; }
.item.active { background: #eff6ff; }
.avatar { width: 32px; height: 32px; border-radius: 50%; background: #2563eb; color: #fff; display: flex; align-items: center; justify-content: center; font-size: 14px; flex: none; }
.name { flex: 1; font-size: 14px; color: #111827; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.badge { min-width: 18px; height: 18px; line-height: 18px; border-radius: 9px; background: #ef4444; color: #fff; font-size: 11px; text-align: center; padding: 0 4px; }
.chat-area { flex: 1; min-width: 0; display: flex; }
.chat-empty { flex: 1; display: flex; align-items: center; justify-content: center; color: #9ca3af; font-size: 14px; }
</style>
