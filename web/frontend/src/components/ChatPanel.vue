<script setup>
// 聊天面板：消息流（history 中性居中 / me 右侧 / peer 左侧）+ 输入框。
// 发送为"发送即忘"（服务端不回响应，见 M2a 核实结论），本地回显自己的消息。
import { ref, watch, nextTick } from 'vue';
import { useChatStore } from '../stores/chat.js';

const props = defineProps({ peer: { type: String, required: true } });
const chat = useChatStore();
const input = ref('');
const error = ref('');
const listEl = ref(null);

async function scrollBottom() {
  await nextTick();
  if (listEl.value) listEl.value.scrollTop = listEl.value.scrollHeight;
}
watch(() => chat.activeMessages.length, scrollBottom);
watch(() => props.peer, scrollBottom);
scrollBottom();

function onSend() {
  error.value = '';
  const text = input.value;
  if (!text.trim()) return;
  try {
    if (chat.send(text)) input.value = '';
  } catch (e) {
    error.value = e.message || '发送失败';
  }
}
function onKeydown(e) {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); onSend(); }
}
</script>

<template>
  <div class="chat-panel">
    <header class="chat-head">与 {{ peer }} 的对话</header>
    <div ref="listEl" class="msgs">
      <div v-if="chat.activeMessages.length === 0" class="no-msg">
        暂无消息。离线期间对方发来的消息会在打开会话时以"离线补发"标注显示。
      </div>
      <div v-for="(m, i) in chat.activeMessages" :key="i" class="row" :class="m.from">
        <div class="bubble">
          <div class="meta">
            <span class="who">{{ m.from === 'me' ? '我' : peer }}</span>
            <span v-if="m.redelivered" class="tag">离线补发</span>
            <span class="time">{{ m.time }}</span>
          </div>
          <div class="text">{{ m.text }}</div>
        </div>
      </div>
    </div>
    <footer class="input-bar">
      <span v-if="error" class="err">{{ error }}</span>
      <div class="input-row">
        <input v-model="input" type="text" placeholder="输入消息，Enter 发送（正文上限 2000 字节）"
               maxlength="2000" @keydown="onKeydown" />
        <button class="send" @click="onSend">发送</button>
      </div>
    </footer>
  </div>
</template>

<style scoped>
.chat-panel { flex: 1; display: flex; flex-direction: column; min-width: 0; }
.chat-head { padding: 12px 16px; background: #fff; border-bottom: 1px solid #e5e7eb; font-size: 14px; font-weight: 600; color: #111827; flex: none; }
.msgs { flex: 1; overflow-y: auto; padding: 16px; display: flex; flex-direction: column; gap: 10px; }
.no-msg { margin: auto; max-width: 360px; text-align: center; font-size: 12px; color: #9ca3af; line-height: 1.7; }
.row { display: flex; }
.row.me { justify-content: flex-end; }
.row.peer { justify-content: flex-start; }
.bubble { max-width: 70%; background: #fff; border-radius: 10px; padding: 8px 12px; box-shadow: 0 1px 2px rgba(0,0,0,.06); }
.row.me .bubble { background: #2563eb; color: #fff; }
.bubble .meta { display: flex; gap: 8px; font-size: 11px; margin-bottom: 3px; opacity: .75; align-items: center; }
.bubble .tag { border: 1px solid currentColor; border-radius: 4px; padding: 0 4px; font-size: 10px; opacity: .9; }
.bubble .text { font-size: 14px; line-height: 1.5; word-break: break-word; white-space: pre-wrap; }
.input-bar { flex: none; background: #fff; border-top: 1px solid #e5e7eb; padding: 10px 16px; }
.input-bar .err { display: block; font-size: 12px; color: #b91c1c; margin-bottom: 6px; }
.input-row { display: flex; gap: 8px; }
.input-row input { flex: 1; padding: 10px 12px; border: 1px solid #d1d5db; border-radius: 8px; font-size: 14px; }
.send { padding: 0 22px; background: #2563eb; color: #fff; border: none; border-radius: 8px; font-size: 14px; cursor: pointer; }
.send:hover { background: #1d4ed8; }
</style>
