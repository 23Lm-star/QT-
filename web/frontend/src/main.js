import { createApp } from 'vue';
import { createPinia } from 'pinia';
import App from './App.vue';
import router from './router.js';
import './style.css';

const app = createApp(App);
app.use(createPinia()); // 必须在 router 之前安装：导航守卫会用到 store
app.use(router);
app.mount('#app');
