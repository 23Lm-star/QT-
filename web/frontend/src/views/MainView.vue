<script setup>
// 主布局：顶部状态栏 + 里程碑页签（M2 仅"好友"可用，其余为占位）+ 好友申请对话框。
import { ref, onMounted, onBeforeUnmount } from 'vue';
import { useRouter } from 'vue-router';
import { useAuthStore } from '../stores/auth.js';
import { useFriendsStore } from '../stores/friends.js';
import { useChatStore } from '../stores/chat.js';
import FriendPanel from '../components/FriendPanel.vue';
import FriendRequestDialog from '../components/FriendRequestDialog.vue';

const auth = useAuthStore();
const friends = useFriendsStore();
const chat = useChatStore();
const router = useRouter();
const tab = ref('friend');

const tabs = [
  { key: 'friend', label: '好友' },
  { key: 'drive', label: '云盘', milestone: 'M3' },
  { key: 'group', label: '群聊', milestone: 'M4' },
  { key: 'share', label: '分享', milestone: 'M5' },
];

onMounted(() => {
  friends.init();
  chat.init();
  friends.loadFriends();
  friends.startSync(); // 10s 轮询 + 聚焦刷新：让"被删方"也能感知好友关系变化
});
onBeforeUnmount(() => {
  friends.stopSync();
});

function onLogout() {
  auth.logout();
  friends.reset();
  chat.reset();
  router.push({ name: 'login' });
}
</script>

<template>
  <div class="main-wrap">
    <header class="bar">
      <span class="app">QT 云盘聊天系统 · 网页端</span>
      <span class="who">已登录：{{ auth.loginName }}</span>
      <span class="conn" :class="auth.connectState">{{ auth.connectState === 'open' ? '在线' : '离线' }}</span>
      <button class="logout" @click="onLogout">退出登录</button>
    </header>

    <nav class="tabs">
      <button v-for="t in tabs" :key="t.key" class="tab" :class="{ active: tab === t.key }" @click="tab = t.key">
        {{ t.label }}
        <span v-if="t.key === 'friend' && chat.totalUnread > 0" class="badge">{{ chat.totalUnread }}</span>
        <span v-if="t.milestone" class="soon">{{ t.milestone }}</span>
      </button>
    </nav>

    <main class="body">
      <FriendPanel v-if="tab === 'friend'" />
      <div v-else class="placeholder">
        <h3>{{ tabs.find((t) => t.key === tab).label }}</h3>
        <p>该模块属于里程碑 {{ tabs.find((t) => t.key === tab).milestone }}，尚未实现。当前分支已完成 M1 登录与 M2 好友私聊。</p>
      </div>
    </main>

    <FriendRequestDialog v-if="friends.incomingRequest" />
  </div>
</template>

<style scoped>
.main-wrap { height: 100vh; display: flex; flex-direction: column; }
.bar { display: flex; align-items: center; gap: 16px; padding: 10px 20px; background: #1f2937; color: #f9fafb; flex: none; }
.bar .app { font-weight: 600; font-size: 14px; }
.bar .who { font-size: 13px; color: #d1d5db; }
.bar .conn { font-size: 12px; padding: 2px 10px; border-radius: 10px; background: #374151; }
.bar .conn.open { background: #166534; }
.bar .logout { margin-left: auto; padding: 6px 14px; border-radius: 6px; border: none; background: #ef4444; color: #fff; cursor: pointer; font-size: 13px; }
.tabs { display: flex; gap: 4px; padding: 8px 16px 0; background: #fff; border-bottom: 1px solid #e5e7eb; flex: none; }
.tab { position: relative; padding: 8px 18px; border: 1px solid transparent; border-bottom: none; border-radius: 8px 8px 0 0; background: transparent; cursor: pointer; font-size: 14px; color: #4b5563; }
.tab.active { background: #f3f4f6; border-color: #e5e7eb; color: #111827; font-weight: 600; }
.tab .soon { margin-left: 6px; font-size: 10px; color: #9ca3af; border: 1px solid #e5e7eb; border-radius: 4px; padding: 0 4px; }
.tab .badge { position: absolute; top: 2px; right: 4px; min-width: 16px; height: 16px; line-height: 16px; border-radius: 8px; background: #ef4444; color: #fff; font-size: 10px; text-align: center; padding: 0 3px; }
.body { flex: 1; min-height: 0; background: #f3f4f6; }
.placeholder { padding: 48px; text-align: center; color: #6b7280; }
</style>
