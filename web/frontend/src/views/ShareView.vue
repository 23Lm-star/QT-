<script setup>
// 分享页（M5 本期范围）：好友文件分享发送侧（33-37）。
// 收到的分享邀请由 MainView 的 ShareInviteDialog 全局处理，本页不重复展示。
// 外链分享（52-57）待服务端修复对象存储文件的取回路径后另行交付，见变更记录 M5 阻塞节。
import { ref, onMounted } from 'vue';
import { useAuthStore } from '../stores/auth.js';
import { useDriveStore } from '../stores/drive.js';
import { useFriendsStore } from '../stores/friends.js';
import { useShareStore } from '../stores/share.js';
import ShareFilePicker from '../components/ShareFilePicker.vue';

const auth = useAuthStore();
const drv = useDriveStore();
const friends = useFriendsStore();
const share = useShareStore();

const pickerOpen = ref(false);
const target = ref({ name: '', absPath: '' });
const selectedFriends = ref([]);

onMounted(async () => {
  refreshFriends();
  // 直达分享页时云盘 store 可能尚未初始化（DriveView 挂载才 enter），先补用户根。
  if (!drv.userRoot) await drv.enter(auth.loginName);
});

async function refreshFriends() {
  await friends.loadFriends();
  await friends.loadOnlineUsers();
}
function isOnline(name) { return friends.onlineUsers.includes(name); }
function toggleFriend(name) {
  const i = selectedFriends.value.indexOf(name);
  if (i >= 0) selectedFriends.value.splice(i, 1);
  else selectedFriends.value.push(name);
}
async function openPicker() {
  if (!drv.userRoot) await drv.enter(auth.loginName);
  pickerOpen.value = true;
}
function onPicked(file) {
  target.value = file;
  pickerOpen.value = false;
}
async function send() {
  const ok = await share.sendToFriends({
    absPath: target.value.absPath,
    receivers: selectedFriends.value.slice(),
  });
  if (ok) {
    target.value = { name: '', absPath: '' };
    selectedFriends.value = [];
  }
}
</script>

<template>
  <div class="share">
    <div v-if="share.hint" class="msg" :class="share.hintType">{{ share.hint }}</div>

    <div class="grid">
      <section class="card">
        <h3>选择文件</h3>
        <p class="tip">选择你云盘中的普通文件；分享只是把内容复制给对方，不会移动你的原文件。</p>
        <div class="filebox">
          <div class="file-name">{{ target.name || '未选择文件' }}</div>
          <div class="file-path" :title="target.absPath">{{ target.absPath || '（仅支持普通文件）' }}</div>
        </div>
        <button class="primary" @click="openPicker">浏览云盘…</button>
      </section>

      <section class="card">
        <h3>选择好友</h3>
        <p class="tip">对方接受后，文件会复制到对方云盘根目录（重名自动改名）。仅在线好友能收到邀请；离线好友的邀请会丢失（与桌面端一致）。</p>
        <div class="friends-head">
          <span>好友（{{ friends.friends.length }}）</span>
          <div class="right">
            <button class="mini" @click="selectedFriends = friends.friends.slice()">全选</button>
            <button class="mini" @click="selectedFriends = []">清空</button>
            <button class="mini" @click="refreshFriends">刷新</button>
          </div>
        </div>
        <ul class="friends">
          <li v-if="friends.friends.length === 0" class="empty">暂无好友（请先在好友页添加）</li>
          <li v-for="n in friends.friends" :key="n" @click="toggleFriend(n)">
            <input type="checkbox" :checked="selectedFriends.includes(n)" @click.stop="toggleFriend(n)" />
            <span class="dot" :class="{ on: isOnline(n) }"></span>
            <span class="name">{{ n }}</span>
            <span class="status">{{ isOnline(n) ? '在线' : '离线' }}</span>
          </li>
        </ul>
        <div class="row">
          <button class="primary send" :disabled="!target.absPath || selectedFriends.length === 0" @click="send">
            发送邀请（{{ selectedFriends.length }}）
          </button>
        </div>
      </section>
    </div>

    <ShareFilePicker
      :open="pickerOpen"
      :start-path="drv.userRoot"
      title="选择要分享给好友的文件"
      @close="pickerOpen = false"
      @select="onPicked"
    />
  </div>
</template>

<style scoped>
.share { height: 100%; overflow-y: auto; padding: 12px 16px; box-sizing: border-box; display: flex; flex-direction: column; gap: 8px; }
.msg { font-size: 13px; padding: 7px 10px; border-radius: 6px; }
.msg.info { background: #eff6ff; color: #1d4ed8; }
.msg.success { background: #ecfdf5; color: #047857; }
.msg.error { background: #fef2f2; color: #b91c1c; }
.grid { display: grid; grid-template-columns: minmax(0, 2fr) minmax(0, 3fr); gap: 12px; align-items: start; }
@media (max-width: 900px) { .grid { grid-template-columns: 1fr; } }
.card { background: #fff; border: 1px solid #e5e7eb; border-radius: 10px; padding: 14px 16px; display: flex; flex-direction: column; gap: 10px; }
h3 { margin: 0; font-size: 15px; color: #111827; }
.tip { margin: 0; font-size: 12px; color: #6b7280; line-height: 1.55; }
.filebox { border: 1px dashed #d1d5db; border-radius: 8px; padding: 14px; background: #f9fafb; }
.file-name { font-size: 14px; color: #111827; font-weight: 600; word-break: break-all; }
.file-path { font-size: 11px; color: #9ca3af; font-family: Consolas, monospace; word-break: break-all; margin-top: 4px; }
button { padding: 6px 12px; font-size: 13px; border: 1px solid #d1d5db; border-radius: 6px; background: #fff; cursor: pointer; color: #374151; }
button:disabled { color: #9ca3af; cursor: not-allowed; }
.primary { background: #2563eb; border-color: #2563eb; color: #fff; }
.primary:disabled { background: #93c5fd; border-color: #93c5fd; color: #fff; }
.send { padding: 8px 20px; }
.friends-head { display: flex; align-items: center; justify-content: space-between; font-size: 12px; color: #6b7280; }
.friends-head .right { display: flex; gap: 4px; }
.mini { padding: 2px 8px; font-size: 11px; border: 1px solid #d1d5db; border-radius: 6px; background: #fff; cursor: pointer; color: #374151; }
.friends { list-style: none; margin: 0; padding: 4px; border: 1px solid #e5e7eb; border-radius: 8px; min-height: 180px; max-height: 340px; overflow-y: auto; }
.friends li { display: flex; align-items: center; gap: 8px; padding: 7px 10px; border-radius: 6px; cursor: pointer; font-size: 13px; }
.friends li:hover { background: #f3f4f6; }
.friends li.empty { color: #9ca3af; cursor: default; justify-content: center; }
.dot { width: 7px; height: 7px; border-radius: 50%; background: #d1d5db; flex: none; }
.dot.on { background: #16a34a; }
.friends .name { word-break: break-all; }
.friends .status { margin-left: auto; font-size: 11px; color: #9ca3af; }
.row { display: flex; justify-content: flex-end; }
</style>
