<script setup>
// 主布局：顶部状态栏 + 四个页签（好友/云盘/群聊/分享）+ 全局邀请弹窗。
import { ref, computed, onMounted, onBeforeUnmount } from 'vue';
import { useRouter } from 'vue-router';
import { useAuthStore } from '../stores/auth.js';
import { useFriendsStore } from '../stores/friends.js';
import { useChatStore } from '../stores/chat.js';
import { useDriveStore } from '../stores/drive.js';
import { useTransferStore } from '../stores/transfer.js';
import { useGroupStore } from '../stores/group.js';
import { useGroupChatStore } from '../stores/groupchat.js';
import { useShareStore } from '../stores/share.js';
import FriendPanel from '../components/FriendPanel.vue';
import DriveView from './DriveView.vue';
import GroupView from './GroupView.vue';
import ShareView from './ShareView.vue';
import FriendRequestDialog from '../components/FriendRequestDialog.vue';
import ShareInviteDialog from '../components/ShareInviteDialog.vue';

const auth = useAuthStore();
const friends = useFriendsStore();
const chat = useChatStore();
const drv = useDriveStore();
const transfer = useTransferStore();
const grp = useGroupStore();
const grpChat = useGroupChatStore();
const share = useShareStore();
const router = useRouter();
const tab = ref('friend');

const tabs = [
  { key: 'friend', label: '好友' },
  { key: 'drive', label: '云盘' },
  { key: 'group', label: '群聊' },
  { key: 'share', label: '分享' },
];

const totalGroupUnread = computed(() =>
  Object.values(grp.unread).reduce((s, n) => s + (n || 0), 0)
);

onMounted(() => {
  friends.init();
  chat.init();
  transfer.init(); // 传输引擎路由与页签无关地接线（云盘/群文件共用，幂等：同一 net 只绑一次）
  share.init();    // 35 分享邀请与页签无关，全局接线
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
  drv.reset();
  transfer.reset();
  grp.reset();
  grpChat.reset();
  share.reset();
  router.push({ name: 'login' });
}
</script>

<template>
  <div class="main-wrap">
    <header class="bar">
      <span class="app">QT 云盘聊天系统 · 网页端</span>
      <span class="who">已登录：{{ auth.loginName }}</span>
      <span class="conn" :class="auth.connectState">{{ auth.connectState === 'open' ? '在线' : '离线' }}</span>
      <span v-if="auth.connectState !== 'open' && auth.lastClose" class="closeinfo" :title="`code=${auth.lastClose.code} reason=${auth.lastClose.reason}`">
        {{ auth.lastClose.text }}
      </span>
      <button class="logout" @click="onLogout">退出登录</button>
    </header>

    <nav class="tabs">
      <button v-for="t in tabs" :key="t.key" class="tab" :class="{ active: tab === t.key }" @click="tab = t.key">
        {{ t.label }}
        <span v-if="t.key === 'friend' && chat.totalUnread > 0" class="badge">{{ chat.totalUnread }}</span>
        <span v-if="t.key === 'group' && totalGroupUnread > 0" class="badge">{{ totalGroupUnread }}</span>
        <span v-if="t.key === 'share' && share.pendingInviteCount > 0" class="badge">{{ share.pendingInviteCount }}</span>
      </button>
    </nav>

    <main class="body">
      <FriendPanel v-if="tab === 'friend'" />
      <DriveView v-else-if="tab === 'drive'" />
      <GroupView v-else-if="tab === 'group'" />
      <ShareView v-else-if="tab === 'share'" />
    </main>

    <FriendRequestDialog v-if="friends.incomingRequest" />
    <ShareInviteDialog />
  </div>
</template>

<style scoped>
.main-wrap { height: 100vh; display: flex; flex-direction: column; }
.bar { display: flex; align-items: center; gap: 16px; padding: 10px 20px; background: #1f2937; color: #f9fafb; flex: none; }
.bar .app { font-weight: 600; font-size: 14px; }
.bar .who { font-size: 13px; color: #d1d5db; }
.bar .conn { font-size: 12px; padding: 2px 10px; border-radius: 10px; background: #374151; }
.bar .conn.open { background: #166534; }
.bar .closeinfo { font-size: 12px; color: #fca5a5; max-width: 46%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.bar .logout { margin-left: auto; padding: 6px 14px; border-radius: 6px; border: none; background: #ef4444; color: #fff; cursor: pointer; font-size: 13px; }
.tabs { display: flex; gap: 4px; padding: 8px 16px 0; background: #fff; border-bottom: 1px solid #e5e7eb; flex: none; }
.tab { position: relative; padding: 8px 18px; border: 1px solid transparent; border-bottom: none; border-radius: 8px 8px 0 0; background: transparent; cursor: pointer; font-size: 14px; color: #4b5563; }
.tab.active { background: #f3f4f6; border-color: #e5e7eb; color: #111827; font-weight: 600; }
.tab .badge { position: absolute; top: 2px; right: 4px; min-width: 16px; height: 16px; line-height: 16px; border-radius: 8px; background: #ef4444; color: #fff; font-size: 10px; text-align: center; padding: 0 3px; }
.body { flex: 1; min-height: 0; background: #f3f4f6; }
</style>
