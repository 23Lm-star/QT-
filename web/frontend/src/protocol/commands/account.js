// 账号域命令（1-4, 44-45）：构造 caData/caMsg 并解析响应。
// 逐字节复刻 Client/client.cpp 的 on_login_PB_clicked / on_register_PB_clicked 与
// msghandler.cpp 的 handllogin / handlrigist / handleCaptcha。
import md5 from 'js-md5';
import { T, LoginRet, CaptchaStatus } from '../msgtypes.js';
import { CA_DATA_SIZE } from '../pdu.js';
import {
  writeFixedStringNul, writeExactBytes, writeBytes, concatBytes,
  readFixedString, readI32, textEncoder, utf8Length,
} from '../fields.js';

// 密码协议哈希：MD5(password 的 UTF-8 字节) 的小写 hex（32 字符）。
// 对齐 passwordHashForProtocol：QCryptographicHash::hash(pwd.toUtf8(), Md5).toHex()。
// 传字节而非字符串给 md5，确保与 Qt 的 UTF-8 编码逐字节一致。
export function passwordHashForProtocol(password) {
  return md5(textEncoder.encode(password)); // js-md5 接受 Uint8Array，返回小写 hex
}

// 组装 caData[64]：[0..31]=用户名(NUL 补零)，[32..63]=MD5 hex(恰好 32 字节)。
function buildAuthCaData(username, passwordHashHex) {
  const caData = new Uint8Array(CA_DATA_SIZE);
  writeFixedStringNul(caData, 0, 32, username);
  writeExactBytes(caData, 32, 32, textEncoder.encode(passwordHashHex));
  return caData;
}

export function validateCredentials(username, password) {
  if (!username || !password) return '用户名或密码不能为空';
  if (utf8Length(username) > 31) return '用户名过长（UTF-8 字节数须 ≤31）';
  return null;
}

// 验证码请求(44)：空 PDU。返回响应解析结果。
export async function requestCaptcha(net) {
  const resp = await net.request(T.CAPTCHA_REQUEST, T.CAPTCHA_RESPOND);
  const status = readI32(resp.caData, 0);
  const remaining = readI32(resp.caData, 4);
  let code = '';
  if (status === CaptchaStatus.OK && resp.uiMsgLen > 0) {
    code = readFixedString(resp.caMsg, 0, resp.uiMsgLen).trim();
  }
  return { status, remaining, code };
}

// 登录请求(3)：caMsg = 验证码 + NUL + 32B clientId。
export async function login(net, { username, password, captcha, clientId }) {
  const err = validateCredentials(username, password);
  if (err) throw new Error(err);
  if (!captcha) throw new Error('请输入验证码');
  const caData = buildAuthCaData(username, passwordHashForProtocol(password));
  const captchaBytes = textEncoder.encode(captcha);
  const clientIdBytes = textEncoder.encode(clientId || '');
  const msg = concatBytes(captchaBytes, new Uint8Array(1), clientIdBytes); // +NUL+clientId
  const resp = await net.request(T.LOGIN_REQUEST, T.LOGIN_RESPOND, caData, msg);
  const ret = readI32(resp.caData, 0);
  const result = { ret };
  if (ret === LoginRet.LOCKED) result.remaining = readI32(resp.caData, 4);
  if (ret === LoginRet.SUCCESS) result.loginName = readFixedString(resp.caData, 32, 32);
  return result;
}

// 注册请求(1)：caMsg = 验证码 + NUL（无 clientId）。
export async function register(net, { username, password, captcha }) {
  const err = validateCredentials(username, password);
  if (err) throw new Error(err);
  if (!captcha) throw new Error('请输入验证码');
  const caData = buildAuthCaData(username, passwordHashForProtocol(password));
  const captchaBytes = textEncoder.encode(captcha);
  const msg = concatBytes(captchaBytes, new Uint8Array(1)); // +NUL
  const resp = await net.request(T.REGIST_REQUEST, T.REGIST_RESPOND, caData, msg);
  const ret = readI32(resp.caData, 0);
  const result = { ret };
  if (ret === LoginRet.LOCKED) result.remaining = readI32(resp.caData, 4);
  // 注册成功：ret 非 0 且非 -2/-3/-4；caData[32..63]=注册名。
  if (ret !== 0 && ret !== -2 && ret !== -3 && ret !== -4) {
    result.name = readFixedString(resp.caData, 32, 32);
    result.success = true;
  } else {
    result.success = false;
  }
  return result;
}

// 把返回码翻译成用户可读提示（登录/注册通用）。
export function describeAuthRet(ret) {
  switch (ret) {
    case LoginRet.SUCCESS: return '成功';
    case LoginRet.BAD_CREDENTIAL: return '账号或密码错误';
    case LoginRet.CAPTCHA_WRONG: return '验证码错误，请重新输入';
    case LoginRet.CAPTCHA_EXPIRED: return '验证码已过期，请重新获取';
    case LoginRet.LOCKED: return '验证码错误次数过多，账号已锁定';
    case 0: return '操作失败';
    default: return `未知返回码 ${ret}`;
  }
}
