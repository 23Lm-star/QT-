<script setup>
// 分享页：外链分享（52-57，含过期/撤销恢复、通过外链取文件）+ 好友文件分享（33-37 发送侧）。
// 收到的分享邀请由 MainView 的 ShareInviteDialog 全局处理，本页不重复展示。
import { ref, computed, onMounted } from 'vue';
import { useAuthStore } from '../stores/auth.js';
import { useDriveStore } from '../stores/drive.js';
import { useFriendsStore } from '../stores/friends.js';
import { useShareStore } from '../stores/share.js';
import { RENEW_DAY_CHOICES, PUBLINK_STATE_REVOKED, PUBLINK_STATE_VALID } from '../protocol/commands/share.js';
import { getUserInfo } from '../protocol/commands/friend.js';
import ShareFilePicker from '../components/ShareFilePicker.vue';

const auth = useAuthStore();
const drv = useDriveStore();
const friends = useFriendsStore();
const share = useShareStore();

const picker = ref({ open: false, mode: 'link', title: '选择云盘文件' });
const friendTarget = ref({ name: '', absPath: '' });
const selectedFriends = ref([]);
const renewDays = ref(30);
const linkInput = ref(null);
const copyHint = ref('');

const stateClass = computed(() => {
  if (share.link.state === PUBLINK_STATE_VALID) return 'ok';
  return share.link.state === PUBLINK_STATE_REVOKED ? 'bad' : 'warn';
});
const canRenew = computed(() => share.link.state !== PUBLINK_STATE_REVOKED);

onMounted(async () => {
  refreshFriends();
  // 直达分享页时云盘 store 可能尚未初始化（DriveView 挂载才 enter），先补用户根。
  if (!drv.userRoot) await drv.enter(auth.loginName);
});

const friendOnline = ref({});   // {名字: 是否在线}，由 46/47 逐好友查询（勿与 friends.onlineNames 混用）

async function refreshFriends() {
  await friends.loadFriends();
  await refreshFriendOnline();
}
// TODO（2026-11-17 登记）：好友在线状态不再借「所有用户」列表——7/8 自始排除好友，
// onlineNames 对好友恒为空 → 在线点恒灰。改走 46/47 逐好友查询（零协议变更、
// 分享页好友量级小，并发 N 次可接受），与 friends.onlineNames 语义彻底解耦。
async function refreshFriendOnline() {
  const results = await Promise.all(friends.friends.map(async (n) => {
    try {
      const info = await getUserInfo(n);
      return [n, info.code === 0 && info.online === 1];
    } catch {
      return [n, false];
    }
  }));
  const next = {};
  for (const [n, on] of results) next[n] = on;
  friendOnline.value = next;
}
function isOnline(name) { return friendOnline.value[name] === true; }
function toggleFriend(name) {
  const i = selectedFriends.value.indexOf(name);
  if (i >= 0) selectedFriends.value.splice(i, 1);
  else selectedFriends.value.push(name);
}

async function openPicker(mode) {
  if (!drv.userRoot) await drv.enter(auth.loginName);
  picker.value = {
    open: true,
    mode,
    title: mode === 'link' ? '选择要生成外链的文件' : '选择要分享给好友的文件',
  };
}
function onPicked(file) {
  if (picker.value.mode === 'link') share.selectLinkTarget(file);
  else friendTarget.value = file;
  picker.value.open = false;
}
async function sendToFriends() {
  const ok = await share.sendToFriends({
    absPath: friendTarget.value.absPath,
    receivers: selectedFriends.value.slice(),
  });
  if (ok) {
    friendTarget.value = { name: '', absPath: '' };
    selectedFriends.value = [];
  }
}

async function copyLink() {
  const text = share.link.link;
  if (!text) return;
  try {
    await navigator.clipboard.writeText(text);
    copyHint.value = '已复制';
  } catch {
    // 非安全上下文（局域网 http）无剪贴板权限：退回选中输入框 + execCommand
    const el = linkInput.value;
    if (el) {
      el.focus();
      el.select();
      try { document.execCommand('copy'); copyHint.value = '已复制'; }
      catch { copyHint.value = '复制失败，请手动选中复制'; }
    }
  }
  setTimeout(() => { copyHint.value = ''; }, 2000);
}
</script>

<template>
  <div class="share">
    <div v-if="share.hint" class="msg" :class="share.hintType">{{ share.hint }}</div>

    <div class="grid">
      <section class="card">
        <h3>外链分享</h3>
        <p class="tip">生成 cloud-disk:// 外链，对方登录后凭链接取回文件（文件复制到其「分享链接」文件夹）。新链接默认 24 小时有效。</p>

        <div class="row">
          <button @click="openPicker('link')">选择文件</button>
          <span class="file" :title="share.link.absPath">{{ share.link.name || '未选择文件' }}</span>
        </div>
        <div class="row">
          <button class="primary" :disabled="!share.link.absPath || share.link.busy" @click="share.fetchOrCreateLink()">
            {{ share.link.busy ? '处理中…' : (share.link.loaded ? '重新查询链接' : '获取/生成链接') }}
          </button>
          <span v-if="share.link.relPath" class="rel">相对路径：{{ share.link.relPath }}</span>
        </div>

        <div v-if="share.link.link" class="linkbox">
          <span class="badge" :class="stateClass">{{ share.linkStateLabel }}</span>
          <input ref="linkInput" class="link" readonly :value="share.link.link" @focus="$event.target.select()" />
          <button @click="copyLink">复制</button>
          <span v-if="copyHint" class="copy-hint">{{ copyHint }}</span>
        </div>

        <div v-if="share.link.exists && share.link.state !== 0" class="recover">
          <p class="warn">
            该链接{{ share.linkStateLabel }}。{{ canRenew ? '可续期恢复，或重新生成一条新链接（旧链接立即失效）。' : '已撤销的链接不能续期，只能重新生成。' }}
          </p>
          <div class="row">
            <button class="primary" :disabled="share.link.busy" @click="share.regenerateLink()">重新生成链接</button>
            <template v-if="canRenew">
              <select v-model.number="renewDays" :disabled="share.link.busy">
                <option v-for="d in RENEW_DAY_CHOICES" :key="d" :value="d">{{ d }} 天</option>
              </select>
              <button :disabled="share.link.busy" @click="share.renewLink(renewDays)">续期</button>
            </template>
          </div>
        </div>

        <hr />

        <h4>通过外链取文件</h4>
        <p class="tip">粘贴完整链接（cloud-disk://share/…）或 token；文件会保存到你的云盘「分享链接」文件夹。</p>
        <div class="row">
          <input class="link" v-model="share.resolveInput" placeholder="cloud-disk://share/… 或 token" @keydown.enter="share.resolveLink()" />
          <button class="primary" :disabled="share.resolveBusy" @click="share.resolveLink()">{{ share.resolveBusy ? '获取中…' : '获取文件' }}</button>
        </div>
        <p v-if="share.resolveResult" class="result" :class="share.resolveResult.ok ? 'ok' : 'bad'">
          {{ share.resolveResult.ok ? `已保存到：${share.resolveResult.savedPath || '分享链接'}` : '分享链接无效或已失效（不存在/已撤销/已过期）' }}
        </p>
      </section>

      <section class="card">
        <h3>分享文件给好友</h3>
        <p class="tip">选择云盘中的普通文件，勾选好友后发送邀请；对方接受后文件复制到对方云盘根目录。</p>

        <div class="row">
          <button @click="openPicker('friend')">选择文件</button>
          <span class="file" :title="friendTarget.absPath">{{ friendTarget.name || '未选择文件' }}</span>
        </div>

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

        <div class="row send-row">
          <button class="primary" :disabled="!friendTarget.absPath || selectedFriends.length === 0" @click="sendToFriends">
            发送邀请（{{ selectedFriends.length }}）
          </button>
          <span class="tip">仅在线好友能收到邀请；离线好友的邀请会丢失（与桌面端一致）。</span>
        </div>
      </section>
    </div>

    <ShareFilePicker
      :open="picker.open"
      :start-path="drv.userRoot"
      :title="picker.title"
      @close="picker.open = false"
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
.grid { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 12px; align-items: start; }
@media (max-width: 900px) { .grid { grid-template-columns: 1fr; } }
.card { background: #fff; border: 1px solid #e5e7eb; border-radius: 10px; padding: 14px 16px; display: flex; flex-direction: column; gap: 8px; }
h3 { margin: 0; font-size: 15px; color: #111827; }
h4 { margin: 4px 0 0; font-size: 14px; color: #111827; }
.tip { margin: 0; font-size: 12px; color: #6b7280; line-height: 1.55; }
.row { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.row button, .recover button, .linkbox button { padding: 6px 12px; font-size: 13px; border: 1px solid #d1d5db; border-radius: 6px; background: #fff; cursor: pointer; color: #374151; }
.row button:hover:not(:disabled), .recover button:hover:not(:disabled), .linkbox button:hover:not(:disabled) { background: #f9fafb; }
button:disabled { color: #9ca3af; cursor: not-allowed; }
.primary { background: #2563eb !important; border-color: #2563eb !important; color: #fff !important; }
.primary:disabled { background: #93c5fd !important; border-color: #93c5fd !important; color: #fff !important; }
.file { font-size: 13px; color: #111827; word-break: break-all; }
.rel { font-size: 11px; color: #9ca3af; font-family: Consolas, monospace; }
.linkbox { display: flex; align-items: center; gap: 8px; background: #f9fafb; border: 1px solid #e5e7eb; border-radius: 8px; padding: 8px; flex-wrap: wrap; }
.link { flex: 1; min-width: 200px; padding: 6px 10px; border: 1px solid #d1d5db; border-radius: 6px; font-size: 12px; font-family: Consolas, monospace; }
.badge { font-size: 11px; padding: 2px 8px; border-radius: 8px; flex: none; }
.badge.ok { background: #ecfdf5; color: #047857; }
.badge.warn { background: #fffbeb; color: #b45309; }
.badge.bad { background: #fef2f2; color: #b91c1c; }
.copy-hint { font-size: 11px; color: #047857; }
.recover { border: 1px dashed #fcd34d; background: #fffbeb; border-radius: 8px; padding: 10px; display: flex; flex-direction: column; gap: 8px; }
.warn { margin: 0; font-size: 12px; color: #92400e; }
select { padding: 6px 8px; border: 1px solid #d1d5db; border-radius: 6px; font-size: 13px; background: #fff; }
hr { border: none; border-top: 1px solid #f3f4f6; margin: 6px 0; width: 100%; }
.result { margin: 0; font-size: 12px; }
.result.ok { color: #047857; }
.result.bad { color: #b91c1c; }
.friends-head { display: flex; align-items: center; justify-content: space-between; font-size: 12px; color: #6b7280; margin-top: 4px; }
.friends-head .right { display: flex; gap: 4px; }
.mini { padding: 2px 8px; font-size: 11px; border: 1px solid #d1d5db; border-radius: 6px; background: #fff; cursor: pointer; color: #374151; }
.friends { list-style: none; margin: 0; padding: 4px; border: 1px solid #e5e7eb; border-radius: 8px; min-height: 160px; max-height: 320px; overflow-y: auto; }
.friends li { display: flex; align-items: center; gap: 8px; padding: 7px 10px; border-radius: 6px; cursor: pointer; font-size: 13px; }
.friends li:hover { background: #f3f4f6; }
.friends li.empty { color: #9ca3af; cursor: default; justify-content: center; }
.dot { width: 7px; height: 7px; border-radius: 50%; background: #d1d5db; flex: none; }
.dot.on { background: #16a34a; }
.friends .name { word-break: break-all; }
.friends .status { margin-left: auto; font-size: 11px; color: #9ca3af; }
.send-row { margin-top: 4px; }
</style>
