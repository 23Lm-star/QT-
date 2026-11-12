<script setup>
import { ref, onMounted, onBeforeUnmount, watch } from 'vue';
import { useRouter } from 'vue-router';
import { useAuthStore } from '../stores/auth.js';

const auth = useAuthStore();
const router = useRouter();
const username = ref('');
const password = ref('');
const captcha = ref('');
const busy = ref(false);

let cooldownTimer = null;
function startCooldownTick() {
  if (cooldownTimer) return;
  cooldownTimer = setInterval(() => {
    if (auth.captchaCooldown > 0) auth.captchaCooldown--;
    if (auth.captchaCooldown <= 0 && cooldownTimer) { clearInterval(cooldownTimer); cooldownTimer = null; }
  }, 1000);
}
// REQ-AUDIT-001 R8：服务端只下发图片，输入框保持空由用户按图填写。
watch(() => auth.captchaImage, (v) => { if (v) startCooldownTick(); });
watch(() => auth.captchaCooldown, (v) => { if (v > 0) startCooldownTick(); });

async function onGetCaptcha() {
  if (auth.captchaCooldown > 0) return;
  await auth.refreshCaptcha();
}
async function onLogin() {
  if (busy.value) return;
  busy.value = true;
  try {
    const r = await auth.login(username.value.trim(), password.value, captcha.value.trim());
    if (r && r.ret === 1) router.push({ name: 'main' });
  } finally { busy.value = false; }
}
async function onRegister() {
  if (busy.value) return;
  busy.value = true;
  try { await auth.register(username.value.trim(), password.value, captcha.value.trim()); }
  finally { busy.value = false; }
}

onMounted(async () => {
  auth.init();
  const ok = await auth.connect();
  if (ok) await auth.refreshCaptcha();
});
onBeforeUnmount(() => { if (cooldownTimer) clearInterval(cooldownTimer); });
</script>

<template>
  <div class="login-wrap">
    <div class="card">
      <h1 class="title">QT 云盘聊天系统</h1>
      <p class="subtitle">网页端 · REQ-WEB-001</p>

      <div class="status" :class="auth.connectState">
        <span class="dot" />
        <span v-if="auth.connectState === 'open'">已连接到服务端</span>
        <span v-else-if="auth.connectState === 'connecting'">正在连接…</span>
        <span v-else-if="auth.connectState === 'closed'">连接已断开</span>
        <span v-else>未连接</span>
      </div>

      <label class="field">
        <span>用户名</span>
        <input v-model="username" type="text" maxlength="31" placeholder="≤31 字节" :disabled="!auth.isConnected" />
      </label>
      <label class="field">
        <span>密码</span>
        <input v-model="password" type="password" placeholder="登录密码" :disabled="!auth.isConnected" />
      </label>
      <label class="field">
        <span>验证码</span>
        <img v-if="auth.captchaImage" class="captcha-img" :src="auth.captchaImage" alt="验证码" />
        <div class="captcha-row">
          <input v-model="captcha" type="text" placeholder="验证码" :disabled="!auth.isConnected" />
          <button class="captcha-btn" :disabled="!auth.isConnected || auth.captchaCooldown > 0" @click="onGetCaptcha">
            {{ auth.captchaCooldown > 0 ? `重新获取 (${auth.captchaCooldown})` : '获取验证码' }}
          </button>
        </div>
      </label>

      <div v-if="auth.message" class="msg" :class="auth.messageType">{{ auth.message }}</div>

      <div class="actions">
        <button class="primary" :disabled="!auth.isConnected || busy" @click="onLogin">登录</button>
        <button class="ghost" :disabled="!auth.isConnected || busy" @click="onRegister">注册</button>
      </div>

      <p class="hint">提示：本系统验证码由服务端下发并回显（与桌面端一致），获取后自动填入。</p>
    </div>
  </div>
</template>

<style scoped>
.login-wrap { min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 24px; }
.card { width: 100%; max-width: 400px; background: #fff; border-radius: 12px; padding: 28px 24px; box-shadow: 0 8px 32px rgba(0,0,0,.12); }
.title { margin: 0; font-size: 22px; color: #1f2937; text-align: center; }
.subtitle { margin: 4px 0 20px; font-size: 12px; color: #9ca3af; text-align: center; }
.status { display: flex; align-items: center; gap: 8px; font-size: 13px; color: #6b7280; margin-bottom: 16px; }
.status .dot { width: 8px; height: 8px; border-radius: 50%; background: #9ca3af; }
.status.open .dot { background: #22c55e; }
.status.connecting .dot { background: #f59e0b; }
.status.closed .dot { background: #ef4444; }
.field { display: block; margin-bottom: 14px; }
.field > span { display: block; font-size: 13px; color: #374151; margin-bottom: 6px; }
.field input { width: 100%; box-sizing: border-box; padding: 10px 12px; border: 1px solid #d1d5db; border-radius: 8px; font-size: 14px; }
.field input:disabled { background: #f3f4f6; color: #9ca3af; }
.captcha-img { display: block; height: 44px; margin: 0 0 8px; border-radius: 6px; border: 1px solid #e5e7eb; background: #fff; }
.captcha-row { display: flex; gap: 8px; }
.captcha-row input { flex: 1; }
.captcha-btn { white-space: nowrap; padding: 0 12px; border: 1px solid #d1d5db; background: #f9fafb; border-radius: 8px; font-size: 13px; cursor: pointer; }
.captcha-btn:disabled { color: #9ca3af; cursor: not-allowed; }
.msg { font-size: 13px; padding: 8px 10px; border-radius: 6px; margin-bottom: 14px; }
.msg.info { background: #eff6ff; color: #1d4ed8; }
.msg.success { background: #ecfdf5; color: #047857; }
.msg.error { background: #fef2f2; color: #b91c1c; }
.actions { display: flex; gap: 10px; }
.actions button { flex: 1; padding: 11px; border-radius: 8px; font-size: 15px; cursor: pointer; border: 1px solid transparent; }
.actions .primary { background: #2563eb; color: #fff; }
.actions .primary:disabled { background: #93c5fd; cursor: not-allowed; }
.actions .ghost { background: #fff; border-color: #d1d5db; color: #374151; }
.actions .ghost:disabled { color: #9ca3af; cursor: not-allowed; }
.hint { margin: 16px 0 0; font-size: 12px; color: #9ca3af; line-height: 1.5; }
</style>
