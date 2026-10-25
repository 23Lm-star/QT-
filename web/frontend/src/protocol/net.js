// NetClient 模块级单例：auth/friends/chat 等 Store 共享同一条连接。
// 放独立模块避免 Store 间循环依赖（auth -> friends -> auth）。
import { NetClient } from './client.js';

let instance = null;

// 取单例；onNew 仅在首次创建时调用一次（auth store 用它挂接连接状态回调）。
export function getNet(onNew) {
  if (!instance) {
    instance = new NetClient();
    if (onNew) onNew(instance);
  }
  return instance;
}

// 仅供测试：替换/注入实例。
export function setNet(n) { instance = n; }
