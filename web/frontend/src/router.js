// 路由：hash 模式，避免网关 SPA 回退复杂度。M1 仅登录页 + 登录后占位主页。
import { createRouter, createWebHashHistory } from 'vue-router';
import LoginView from './views/LoginView.vue';
import MainView from './views/MainView.vue';
import { useAuthStore } from './stores/auth.js';

const routes = [
  { path: '/', name: 'login', component: LoginView, meta: { guestOnly: true } },
  { path: '/main', name: 'main', component: MainView, meta: { requiresAuth: true } },
];

const router = createRouter({ history: createWebHashHistory(), routes });

router.beforeEach((to) => {
  const auth = useAuthStore();
  if (to.meta.requiresAuth && !auth.loggedIn) return { name: 'login' };
  if (to.meta.guestOnly && auth.loggedIn) return { name: 'main' };
  return true;
});

export default router;
