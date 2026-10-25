import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

// 开发：页面由 Vite 提供（:5173），/ws 反代到网关（:8080），前端连同源 ws://host/ws 即可。
// 生产：`npm run build` 产物落 dist/，由网关同源托管，/ws 直连网关。
export default defineConfig({
  plugins: [vue()],
  server: {
    port: 5173,
    proxy: {
      '/ws': {
        target: 'ws://127.0.0.1:8080',
        ws: true,
        changeOrigin: true,
      },
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
});
