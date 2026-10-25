<script setup>
// 群聊天面板：渲染选中群消息、发送、翻页加载更早、失败重发。数据来自 groupChat store。
// 自己的消息右气泡、他人左气泡带发送者名；系统消息（msgType=1）居中灰字；
// 发送中/失败有状态标记，失败点击重发（复用 client_msg_id）。
import { ref, watch, nextTick, onMounted } from 'vue';
import { useGroupChatStore } from '../stores/groupchat.js';
import { GROUP_MSG_MAX_BYTES } from '../protocol/commands/group.js';

const props = defineProps({
  groupId: { type: [String, Object], default: null },
  groupName: { type: String, default: '' },
});
const chat = useGroupChatStore();
const draft = ref('');
const scrollEl = ref(null);
const error = ref('');

function scrollBottom() {
  nextTick(() => { if (scrollEl.value) scrollEl.value.scrollTop = scrollEl.value.scrollHeight; });
}

watch(() => props.groupId, async (gid) => {
  if (gid != null) { error.value = ''; await chat.openGroup(gid); scrollBottom(); }
}, { immediate: true });

watch(() => chat.activeMessages.length, scrollBottom);

async function doSend() {
  const body = draft.value.trim();
  if (!body) return;
  if (new TextEncoder().encode(body).length > GROUP_MSG_MAX_BYTES) {
    error.value = `消息过长，上限 ${GROUP_MSG_MAX_BYTES} 字节`;
    return;
  }
  error.value = '';
  await chat.send(body);
  draft.value = '';
}
function onEnter(e) {
  if (e.shiftKey) return; // Shift+Enter 换行
  e.preventDefault();
  doSend();
}
</script>

<template>
  <div class="chat">
    <div class="chat-head">
      <span>{{ groupName }}</span>
      <button v-if="chat.activeMessages.length && !chat.loading" class="older" @click="chat.loadOlder()">加载更早</button>
    </div>
    <div ref="scrollEl" class="msgs">
      <div v-for="(m, i) in chat.activeMessages" :key="m.seq || ('tmp'+i)" class="line">
        <!-- 系统消息居中 -->
        <div v-if="m.msgType === 1" class="sys">{{ m.content }}</div>
        <!-- 普通消息 -->
        <div v-else class="bubble-row" :class="{ mine: m.mine }">
          <div v-if="!m.mine" class="who">{{ m.sender }}</div>
          <div class="bubble" :class="[m.mine ? 'me' : 'peer', m.state]">
            <div class="text">{{ m.content }}</div>
            <div class="meta">
              <span>{{ m.time }}</span>
              <span v-if="m.state === 'sending'" class="st sending">发送中…</span>
              <span v-else-if="m.state === 'failed'" class="st failed" @click="chat.retry(m.clientMsgId)">发送失败，点此重发</span>
            </div>
          </div>
        </div>
      </div>
      <div v-if="chat.loading" class="tip">加载历史…</div>
    </div>
    <div v-if="error" class="err">{{ error }}</div>
    <div class="composer">
      <textarea
        v-model="draft"
        class="input"
        placeholder="输入群消息，Enter 发送，Shift+Enter 换行"
        rows="2"
        @keydown.enter="onEnter"
      ></textarea>
      <button class="send" @click="doSend">发送</button>
    </div>
  </div>
</template>

<style scoped>
.chat { flex: 1; min-height: 0; display: flex; flex-direction: column; }
.chat-head { display: flex; align-items: center; justify-content: space-between; padding: 10px 16px; background: #fff; border-bottom: 1px solid #e5e7eb; font-weight: 600; font-size: 14px; color: #111827; flex: none; }
.older { padding: 3px 10px; font-size: 12px; border: 1px solid #d1d5db; border-radius: 5px; background: #fff; color: #374151; cursor: pointer; }
.msgs { flex: 1; overflow: auto; padding: 14px 16px; display: flex; flex-direction: column; gap: 10px; }
.sys { text-align: center; color: #9ca3af; font-size: 12px; }
.bubble-row { display: flex; align-items: flex-end; gap: 8px; }
.bubble-row.mine { justify-content: flex-end; }
.who { font-size: 11px; color: #6b7280; max-width: 80px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.bubble { max-width: 68%; padding: 8px 12px; border-radius: 10px; word-break: break-word; }
.bubble.peer { background: #fff; border: 1px solid #e5e7eb; }
.bubble.me { background: #dbeafe; }
.bubble.failed { opacity: .7; }
.text { font-size: 14px; color: #111827; }
.meta { display: flex; gap: 8px; margin-top: 4px; font-size: 10px; color: #9ca3af; justify-content: flex-end; }
.st.sending { color: #6b7280; }
.st.failed { color: #b91c1c; cursor: pointer; }
.tip { text-align: center; color: #9ca3af; font-size: 12px; }
.err { padding: 4px 16px; color: #b91c1c; font-size: 12px; background: #fef2f2; flex: none; }
.composer { display: flex; gap: 8px; padding: 10px 16px; background: #fff; border-top: 1px solid #e5e7eb; flex: none; align-items: flex-end; }
.input { flex: 1; resize: none; border: 1px solid #d1d5db; border-radius: 8px; padding: 8px 10px; font-size: 14px; font-family: inherit; }
.send { padding: 8px 18px; border: none; border-radius: 8px; background: #2563eb; color: #fff; cursor: pointer; font-size: 14px; }
</style>
