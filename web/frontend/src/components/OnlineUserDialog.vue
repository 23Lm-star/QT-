<script setup>
// 在线用户对话框：列出在线用户（服务端已排除自己），可发送好友申请。
import { useFriendsStore } from '../stores/friends.js';

const emit = defineEmits(['close']);
const friends = useFriendsStore();

function isFriend(name) { return friends.friends.includes(name); }
function onAdd(name) { friends.addFriend(name); }
</script>

<template>
  <div class="mask" @click.self="emit('close')">
    <div class="dialog">
      <header>
        <span>在线用户</span>
        <div class="head-btns">
          <button @click="friends.loadOnlineUsers()">刷新</button>
          <button class="close" @click="emit('close')">✕</button>
        </div>
      </header>
      <ul>
        <li v-if="friends.onlineUsers.length === 0" class="empty">当前没有其他在线用户</li>
        <li v-for="name in friends.onlineUsers" :key="name">
          <span class="dot" />
          <span class="name">{{ name }}</span>
          <button :disabled="isFriend(name)" @click="onAdd(name)">
            {{ isFriend(name) ? '已是好友' : '加好友' }}
          </button>
        </li>
      </ul>
      <p class="note">发送后需等待对方同意；对方不在线时会提示失败。</p>
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
.name { flex: 1; font-size: 14px; color: #111827; }
li button { border: 1px solid #bfdbfe; background: #eff6ff; color: #1d4ed8; border-radius: 6px; padding: 4px 12px; font-size: 12px; cursor: pointer; }
li button:disabled { border-color: #e5e7eb; background: #f3f4f6; color: #9ca3af; cursor: default; }
.note { margin: 0; padding: 10px 16px; border-top: 1px solid #f3f4f6; font-size: 12px; color: #9ca3af; }
</style>
