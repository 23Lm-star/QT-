// 账号域命令（1-4, 44-45）：构造 caData/caMsg 并解析响应。
// 逐字节复刻 Client/client.cpp 的 on_login_PB_clicked / on_register_PB_clicked 与
// msghandler.cpp 的 handllogin / handlrigist / handleCaptcha。
import { T, LoginRet, CaptchaStatus, protocolCodeText } from '../msgtypes.js';
import { CA_DATA_SIZE } from '../pdu.js';
import {
  writeFixedStringNul, writeBytes, concatBytes,
  readFixedString, readI32, textEncoder, utf8Length,
} from '../fields.js';

// 组装 caData[64]：[0..31]=用户名(NUL 补零)，[32..63]=UTF-8 明文密码。
function buildAuthCaData(username, password) {
  const caData = new Uint8Array(CA_DATA_SIZE);
  writeFixedStringNul(caData, 0, 32, username);
  writeFixedStringNul(caData, 32, 32, password);
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
  // REQ-AUDIT-001 R8：默认载荷是 PNG 图片（文本答案只在服务端）。
  // 仅当部署显式开启测试旁路（服务端回文本）时，才同时回带 code 供自动化套件使用。
  let image = '';
  let code = '';
  if (status === CaptchaStatus.OK && resp.uiMsgLen > 0) {
    const bytes = new Uint8Array(resp.caMsg, 0, resp.uiMsgLen);
    const isPng = bytes.length > 8 && bytes[0] === 0x89 && bytes[1] === 0x50
      && bytes[2] === 0x4e && bytes[3] === 0x47;
    if (isPng) {
      let binary = '';
      for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]);
      image = `data:image/png;base64,${btoa(binary)}`;
    } else {
      code = readFixedString(resp.caMsg, 0, resp.uiMsgLen).trim();
    }
  }
  return { status, remaining, image, code };
}

// 登录请求(3)：caMsg = 验证码 + NUL + 32B clientId。
export async function login(net, { username, password, captcha, clientId }) {
  const err = validateCredentials(username, password);
  if (err) throw new Error(err);
  if (!captcha) throw new Error('请输入验证码');
  const caData = buildAuthCaData(username, password);
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
  const caData = buildAuthCaData(username, password);
  const captchaBytes = textEncoder.encode(captcha);
  const msg = concatBytes(captchaBytes, new Uint8Array(1)); // +NUL
  const resp = await net.request(T.REGIST_REQUEST, T.REGIST_RESPOND, caData, msg);
  const ret = readI32(resp.caData, 0);
  const result = { ret };
  if (ret === LoginRet.LOCKED) result.remaining = readI32(resp.caData, 4);
  // REQ-PROTO-CODE-001 标准码：0=成功（旧「ret 非 0 即成功」判定与服务端标准码相反，
  // 导致网页注册对标准码服务端必失败——002 回归时发现并修复）；caData[32..63]=注册名。
  result.success = ret === LoginRet.SUCCESS;
  if (result.success) result.name = readFixedString(resp.caData, 32, 32);
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
    case LoginRet.ACCOUNT_EXISTS: return '账号已存在';
    case LoginRet.DB_UNAVAILABLE: return '数据库不可用，请联系服务端';
    default: return protocolCodeText(ret);
  }
}
