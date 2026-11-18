<script setup>
// 所有用户对话框（REQ-PAGE-FETCH-001，原"在线用户"）：列出全部可添加的陌生用户
// （含离线；服务端已排除自己与好友），点标在线状态，可发送好友申请。
// REQ-PAGE-FETCH-002：服务端真分页（加载更多）＋用户名子串搜索＋总量封顶提示。
import { ref } from 'vue';
import { useFriendsStore } from '../stores/friends.js';
import { utf8Length } from '../protocol/fields.js';
import { LIST_TOTAL_CAP } from '../protocol/commands/friend.js';

const emit = defineEmits(['close']);
const friends = useFriendsStore();
const kw = ref(friends.onlineKeyword);
const searchErr = ref('');

function isFriend(name) { return friends.friends.includes(name); }
function onAdd(name) { friends.addFriend(name); }
// 搜索/清空均从第 0 页重拉；超限关键词客户端拦截不发送（与桌面端同口径）
function doSearch() {
  const k = kw.value.trim();
  if (utf8Length(k) > 31) { searchErr.value = '关键词过长（UTF-8 须 ≤31 字节）'; return; }
  searchErr.value = '';
  friends.searchOnlineUsers(k);
}
</script>

<template>
  <div class="mask" @click.self="emit('close')">
    <div class="dialog">
      <header>
        <span>所有用户</span>
        <div class="head-btns">
          <button @click="friends.loadOnlineUsers()">刷新</button>
          <button class="close" @click="emit('close')">✕</button>
        </div>
      </header>
      <div class="search-row">
        <input v-model="kw" type="text" placeholder="搜索用户名（子串匹配，回车搜索）" @keyup.enter="doSearch" />
        <button @click="doSearch">搜索</button>
      </div>
      <p v-if="searchErr" class="search-err">{{ searchErr }}</p>
      <ul>
        <li v-if="friends.onlineUsers.length === 0" class="empty">
          {{ friends.onlineKeyword ? '没有匹配的用户' : '当前没有可添加的用户' }}
        </li>
        <li v-for="entry in friends.onlineUsers" :key="entry.name">
          <span class="dot" :class="{ offline: !entry.online }" />
          <span class="name">{{ entry.name }}（{{ entry.online ? '在线' : '离线' }}）</span>
          <button :disabled="isFriend(entry.name)" @click="onAdd(entry.name)">
            {{ isFriend(entry.name) ? '已是好友' : '加好友' }}
          </button>
        </li>
      </ul>
      <div class="foot">
        <span class="count">已显示 {{ friends.onlineUsers.length }}/{{ friends.onlineTotal }} 人</span>
        <button v-if="friends.onlineHasMore" class="more" @click="friends.loadMoreOnlineUsers()">加载更多</button>
      </div>
      <p v-if="friends.onlineCapped" class="note capped">结果超过 {{ LIST_TOTAL_CAP }} 条，仅显示前 {{ LIST_TOTAL_CAP }} 条。</p>
      <p class="note">列表含离线用户；发送后需等待对方同意，对方不在线时申请会登记待处理。</p>
    </div>
  </div>
</template>

<style scoped>
.mask { position: fixed; inset: 0; background: rgba(0,0,0,.35); display: flex; align-items: center; justify-content: center; z-index: 50; }
.dialog { width: 380px; max-height: 70vh; background: #fff; border-radius: 12px; display: flex; flex-direction: column; overflow: hidden; }
header { display: flex; align-items: center; justify-content: space-between; padding: 12px 16px; border-bottom: 1px solid #e5e7eb; font-weight: 600; font-size: 14px; }
.head-btns { display: flex; gap: 6px; }
.head-btns button { border: 1px solid #d1d5db; background: #fff; border-radius: 6px; padding: 3px 10px; font-size: 12px; cursor: pointer; }
.head-btns .close { border: none; font-size: 14px; }
ul { list-style: none; margin: 0; padding: 8px; overflow-y: auto; }
ul .empty { padding: 20px; text-align: center; color: #9ca3af; font-size: 13px; }
li { display: flex; align-items: center; gap: 10px; padding: 8px 10px; border-radius: 8px; }
li:hover { background: #f9fafb; }
.dot { width: 8px; height: 8px; border-radius: 50%; background: #22c55e; flex: none; }
.dot.offline { background: #d1d5db; }
.name { flex: 1; font-size: 14px; color: #111827; }
li button { border: 1px solid #bfdbfe; background: #eff6ff; color: #1d4ed8; border-radius: 6px; padding: 4px 12px; font-size: 12px; cursor: pointer; }
li button:disabled { border-color: #e5e7eb; background: #f3f4f6; color: #9ca3af; cursor: default; }
.note { margin: 0; padding: 10px 16px; border-top: 1px solid #f3f4f6; font-size: 12px; color: #9ca3af; }
.note.capped { color: #b45309; border-top: none; padding-bottom: 0; }
.search-row { display: flex; gap: 6px; padding: 8px 16px; border-bottom: 1px solid #f3f4f6; }
.search-row input { flex: 1; border: 1px solid #d1d5db; border-radius: 6px; padding: 4px 8px; font-size: 13px; }
.search-row button { border: 1px solid #bfdbfe; background: #eff6ff; color: #1d4ed8; border-radius: 6px; padding: 4px 12px; font-size: 12px; cursor: pointer; }
.search-err { margin: 0; padding: 4px 16px; font-size: 12px; color: #b91c1c; }
.foot { display: flex; align-items: center; justify-content: space-between; padding: 6px 16px; border-top: 1px solid #f3f4f6; }
.foot .count { font-size: 12px; color: #6b7280; }
.foot .more { border: 1px solid #d1d5db; background: #fff; border-radius: 6px; padding: 3px 12px; font-size: 12px; cursor: pointer; }
</style>
