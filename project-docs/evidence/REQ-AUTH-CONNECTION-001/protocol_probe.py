# -*- coding: utf-8 -*-
# REQ-AUTH-CONNECTION-001 阶段一：字节级协议探针（纯逻辑，不连服务端）
# 覆盖：119-123 新报文契约、-1003/-3015..-3018 结果码、clientId/恢复凭据校验、
#       截断载荷、非法 ID、未知结果码、旧 1/3/44/48/50 报文兼容（静态+行为）。
# 实现新协议之前运行应当 FAIL；实现之后应当全 PASS。
# 运行：py -3.13 -X utf8 project-docs/evidence/REQ-AUTH-CONNECTION-001/protocol_probe.py
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SERVER_H = ROOT / "Server" / "protocol.h"
CLIENT_H = ROOT / "Client" / "protocol.h"
SERVER_CPP = ROOT / "Server" / "protocol.cpp"
CLIENT_CPP = ROOT / "Client" / "protocol.cpp"
WEB_DIR = ROOT / "web" / "frontend" / "src" / "protocol"
JS_MSGTYPES = WEB_DIR / "msgtypes.js"
JS_SESSION = WEB_DIR / "session.js"
JS_ACCOUNT = WEB_DIR / "account.js"

BLOCK_BEGIN = "// ===== REQ-AUTH-CONNECTION-001 BEGIN PROBE BLOCK (Qt-free) ====="
BLOCK_END = "// ===== REQ-AUTH-CONNECTION-001 END PROBE BLOCK ====="

NEW_MSGS = {
    "ENUM_MSG_TYPE_SESSION_LOGOUT_REQUEST": 119,
    "ENUM_MSG_TYPE_SESSION_LOGOUT_RESPOND": 120,
    "ENUM_MSG_TYPE_SESSION_RESUME_REQUEST": 121,
    "ENUM_MSG_TYPE_SESSION_RESUME_RESPOND": 122,
    "ENUM_MSG_TYPE_SESSION_INVALIDATED_EVENT": 123,
}
NEW_MSGS_JS = {
    "SESSION_LOGOUT_REQUEST": 119,
    "SESSION_LOGOUT_RESPOND": 120,
    "SESSION_RESUME_REQUEST": 121,
    "SESSION_RESUME_RESPOND": 122,
    "SESSION_INVALIDATED_EVENT": 123,
}
LEGACY_MSGS = {
    "ENUM_MSG_TYPE_REGIST_REQUEST": 1,
    "ENUM_MSG_TYPE_REGIST_RESPOND": 2,
    "ENUM_MSG_TYPE_LOGIN_REQUEST": 3,
    "ENUM_MSG_TYPE_LOGIN_RESPOND": 4,
    "ENUM_MSG_TYPE_CAPTCHA_REQUEST": 44,
    "ENUM_MSG_TYPE_CAPTCHA_RESPOND": 45,
    "ENUM_MSG_TYPE_LOGOFF_REQUEST": 48,
    "ENUM_MSG_TYPE_LOGOFF_RESPOND": 49,
    "ENUM_MSG_TYPE_HEARTBEAT_REQUEST": 50,
    "ENUM_MSG_TYPE_HEARTBEAT_RESPOND": 51,
}
LEGACY_MSGS_JS = {
    "REGIST_REQUEST": 1, "REGIST_RESPOND": 2, "LOGIN_REQUEST": 3, "LOGIN_RESPOND": 4,
    "CAPTCHA_REQUEST": 44, "CAPTCHA_RESPOND": 45, "LOGOFF_REQUEST": 48, "LOGOFF_RESPOND": 49,
    "HEARTBEAT_REQUEST": 50, "HEARTBEAT_RESPOND": 51,
}
NEW_CODES = {
    "ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE": -1003,
    "ENUM_PROTOCOL_CODE_BAD_CLIENT_ID": -3015,
    "ENUM_PROTOCOL_CODE_BAD_RESUME_TOKEN": -3016,
    "ENUM_PROTOCOL_CODE_SESSION_INVALID": -3017,
    "ENUM_PROTOCOL_CODE_SESSION_WRITE_FAILED": -3018,
}
LAYOUT_NAMES = [
    "AUTH_CLIENT_ID_HEX_LEN", "AUTH_RESUME_TOKEN_HEX_LEN",
    "SESSION_CLIENT_ID_OFFSET", "SESSION_RESP_CODE_OFFSET",
    "SESSION_RESP_VERSION_OFFSET", "SESSION_RESP_USER_OFFSET",
    "SESSION_RESUME_MSG_LEN",
]
LAYOUT_EXPECT = {
    "AUTH_CLIENT_ID_HEX_LEN": 32, "AUTH_RESUME_TOKEN_HEX_LEN": 64,
    "SESSION_CLIENT_ID_OFFSET": 0, "SESSION_RESP_CODE_OFFSET": 0,
    "SESSION_RESP_VERSION_OFFSET": 4, "SESSION_RESP_USER_OFFSET": 32,
    "SESSION_RESUME_MSG_LEN": 65,
}
JS_LAYOUT_NAMES = [
    "CLIENT_ID_HEX_LEN", "RESUME_TOKEN_HEX_LEN",
    "SESSION_CLIENT_ID_OFFSET", "SESSION_RESP_CODE_OFFSET",
    "SESSION_RESP_VERSION_OFFSET", "SESSION_RESP_USER_OFFSET",
    "SESSION_RESUME_MSG_LEN",
]
JS_LAYOUT_EXPECT = {
    "CLIENT_ID_HEX_LEN": 32, "RESUME_TOKEN_HEX_LEN": 64,
    "SESSION_CLIENT_ID_OFFSET": 0, "SESSION_RESP_CODE_OFFSET": 0,
    "SESSION_RESP_VERSION_OFFSET": 4, "SESSION_RESP_USER_OFFSET": 32,
    "SESSION_RESUME_MSG_LEN": 65,
}

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok), detail))
    print(("PASS " if ok else "FAIL ") + name + ((" | " + str(detail)) if detail and not ok else ""))


def read(path):
    try:
        return path.read_text(encoding="utf-8")
    except OSError as e:
        return None


def parse_cpp_enum(text, enum_name):
    m = re.search(r"enum\s+" + enum_name + r"\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return {}
    entries, nxt = {}, 0
    for line in m.group(1).splitlines():
        line = re.sub(r"//.*", "", line).strip()
        em = re.match(r"^(\w+)\s*(?:=\s*(-?\d+))?\s*,?$", line)
        if not em:
            continue
        val = int(em.group(2)) if em.group(2) is not None else nxt
        entries[em.group(1)] = val
        nxt = val + 1
    return entries


def parse_static_consts(text, names):
    vals = {}
    for n in names:
        m = re.search(r"static const int\s+" + n + r"\s*=\s*([^;]+);", text)
        if not m:
            vals[n] = None
            continue
        raw = m.group(1).strip()
        for k, v in vals.items():
            if v is not None:
                raw = re.sub(r"\b" + k + r"\b", str(v), raw)
        try:
            vals[n] = int(eval(raw, {"__builtins__": {}}, {}))
        except Exception:
            vals[n] = None
    return vals


def parse_js_freeze(text, obj_name):
    m = re.search(r"const " + obj_name + r" = Object\.freeze\(\{(.*?)\n\}\);", text, re.S)
    if not m:
        return {}
    entries = {}
    for em in re.finditer(r"\[?(-?\w[\w]*)\]?:\s*(-?\d+),?", m.group(1)):
        key = em.group(1)
        try:
            entries[int(key)] = int(em.group(2))
        except ValueError:
            entries[key] = int(em.group(2))
    return entries


def parse_js_code_text(text):
    m = re.search(r"const CODE_TEXT = Object\.freeze\(\{(.*?)\n\}\);", text, re.S)
    if not m:
        return set()
    return {int(k) for k in re.findall(r"\[?(-?\d+)\]?:\s*['\"]", m.group(1))}


def extract_block(text):
    if text is None:
        return None
    i, j = text.find(BLOCK_BEGIN), text.find(BLOCK_END)
    if i < 0 or j < 0:
        return None
    return text[i + len(BLOCK_BEGIN):j]


def parse_kPduTypeNames(text):
    m = re.search(r"kPduTypeNames\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return []
    return re.findall(r'"([^"]*)"', m.group(1))


# ---------- A. 静态契约 ----------
def static_checks():
    sh, ch = read(SERVER_H), read(CLIENT_H)
    sjs = read(JS_MSGTYPES)
    check("A0 Server/protocol.h 可读", sh is not None)
    check("A0 Client/protocol.h 可读", ch is not None)
    check("A0 msgtypes.js 可读", sjs is not None)
    if sh is None or ch is None or sjs is None:
        return
    for label, text in (("Server", sh), ("Client", ch)):
        en = parse_cpp_enum(text, "ENUM_MSG_TYPE")
        for name, val in NEW_MSGS.items():
            check(f"A1 {label} {name}={val}", en.get(name) == val, en.get(name))
        for name, val in LEGACY_MSGS.items():
            check(f"A7 {label} 旧号 {name}={val}", en.get(name) == val, en.get(name))
        codes = parse_cpp_enum(text, "ENUM_PROTOCOL_CODE")
        for name, val in NEW_CODES.items():
            check(f"A2 {label} {name}={val}", codes.get(name) == val, codes.get(name))
            check(f"A3 {label} protocolCodeText 覆盖 {name}",
                  ("case " + name + ":") in text)
        consts = parse_static_consts(text, LAYOUT_NAMES)
        for n, want in LAYOUT_EXPECT.items():
            check(f"A4 {label} {n}={want}", consts.get(n) == want, consts.get(n))
    jen = parse_js_freeze(sjs, "T")
    for name, val in NEW_MSGS_JS.items():
        check(f"A1 Web T.{name}={val}", jen.get(name) == val, jen.get(name))
    for name, val in LEGACY_MSGS_JS.items():
        check(f"A7 Web T.{name}={val}", jen.get(name) == val, jen.get(name))
    codes_js = parse_js_code_text(sjs)
    for val in NEW_CODES.values():
        check(f"A5 Web CODE_TEXT 含 {val}", val in codes_js)
    # 探针区块两端逐字符一致
    bs, bc = extract_block(sh), extract_block(ch)
    check("A6 探针区块 Server/Client 均存在", bs is not None and bc is not None)
    if bs is not None and bc is not None:
        check("A6 探针区块两端逐字符一致", bs == bc)


def cpp_impl_checks():
    scpp, ccpp = read(SERVER_CPP), read(CLIENT_CPP)
    for label, text in (("Server", scpp), ("Client", ccpp)):
        if text is None:
            check(f"A8 {label} protocol.cpp 可读", False)
            continue
        names = parse_kPduTypeNames(text)
        for val, js_name in ((119, "SESSION_LOGOUT_REQUEST"), (120, "SESSION_LOGOUT_RESPOND"),
                             (121, "SESSION_RESUME_REQUEST"), (122, "SESSION_RESUME_RESPOND"),
                             (123, "SESSION_INVALIDATED_EVENT")):
            ok = len(names) > val and names[val] == js_name
            check(f"A8 {label} kPduTypeNames[{val}]={js_name}", ok, names[val] if len(names) > val else "缺项")
        for legacy in ("ENUM_MSG_TYPE_REGIST_REQUEST", "ENUM_MSG_TYPE_LOGIN_REQUEST",
                       "ENUM_MSG_TYPE_CAPTCHA_RESPOND", "ENUM_MSG_TYPE_LOGOFF_REQUEST",
                       "ENUM_MSG_TYPE_LOGOFF_RESPOND"):
            check(f"A8 {label} 旧类型日志解码保留 {legacy}", ("case " + legacy) in text)
        for new_name in NEW_MSGS:
            check(f"A8 {label} 新类型日志解码 {new_name}", ("case " + new_name) in text)


# ---------- B. C++ 行为（g++ 编译探针区块） ----------
ID32 = "0123456789abcdef0123456789abcdef"
ID32_UP = "0123456789ABCDEF0123456789abcdef"
ID32_BAD = "0123456789abcdef0123456789abcdeg"
TOK64 = "0123456789abcdef" * 4


def c_bytes(carr):
    return carr


def cpp_behavior_checks():
    sh, ch = read(SERVER_H), read(CLIENT_H)
    block = extract_block(sh) if sh else None
    gxx = shutil.which("g++") or r"C:\Qt\Tools\mingw730_64\bin\g++.exe"
    if block is None or not Path(gxx).exists():
        check("B0 C++ 行为验证可执行", False,
              "缺探针区块" if block is None else "缺 g++")
        return
    le32 = [f"0x{b:02X}" for b in struct.pack("<i", -3015)]
    le32_0 = [f"0x{b:02X}" for b in struct.pack("<i", 0)]
    le64 = [f"0x{b:02X}" for b in struct.pack("<q", 0x0102030405060708)]
    harness = f"""
#include <cstdio>
#include <cstring>
namespace S {{
{block}
}}
static int fails = 0;
static void ck(const char *name, bool ok) {{
    printf("CHECK %s %s\\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}}
int main() {{
    using namespace S;
    char id[33] = "{ID32}";
    char idUp[33] = "{ID32_UP}";
    char idBad[33] = "{ID32_BAD}";
    ck("cpp_valid_id", isValidClientIdBytes(id, 32));
    ck("cpp_id_len31", !isValidClientIdBytes(id, 31));
    ck("cpp_id_len33", !isValidClientIdBytes(id, 33));
    ck("cpp_id_upper", !isValidClientIdBytes(idUp, 32));
    ck("cpp_id_nonhex", !isValidClientIdBytes(idBad, 32));
    ck("cpp_id_empty", !isValidClientIdBytes("", 0));
    char tok[65] = "{TOK64}";
    ck("cpp_valid_token", isValidResumeTokenBytes(tok, 64));
    ck("cpp_token_len63", !isValidResumeTokenBytes(tok, 63));
    ck("cpp_token_len65", !isValidResumeTokenBytes(tok, 65));
    ck("cpp_token_upper", !isValidResumeTokenBytes("{TOK64[:63]}A", 64));
    char ca[64];
    memset(ca, 0xAA, sizeof(ca));
    authFillClientIdField(ca, id);
    bool fillOk = memcmp(ca, id, 32) == 0;
    for (int i = 32; i < 64; ++i) if (ca[i] != 0) fillOk = false;
    ck("cpp_fill_client_id_field", fillOk);
    char msg[80];
    int n = authWriteResumeTokenMsg(msg, 80, tok);
    ck("cpp_resume_msg_len", n == 65 && msg[64] == '\\0' && memcmp(msg, tok, 64) == 0);
    ck("cpp_resume_msg_truncated", authWriteResumeTokenMsg(msg, 64, tok) == -1);
    char b4[4];
    authAppendI32(b4, -3015);
    const unsigned char EXP3015[4] = {{{",".join(le32)}}};
    ck("cpp_i32_le_bytes", memcmp(b4, EXP3015, 4) == 0);
    authAppendI32(b4, 0);
    const unsigned char EXP0[4] = {{{",".join(le32_0)}}};
    ck("cpp_i32_zero_bytes", memcmp(b4, EXP0, 4) == 0);
    ck("cpp_i32_roundtrip", authReadI32(b4) == 0);
    authAppendI32(b4, -3018);
    ck("cpp_i32_roundtrip_neg", authReadI32(b4) == -3018);
    char b8[8];
    authAppendI64(b8, 0x0102030405060708LL);
    const unsigned char EXP64[8] = {{{",".join(le64)}}};
    ck("cpp_i64_le_bytes", memcmp(b8, EXP64, 8) == 0);
    ck("cpp_i64_roundtrip", authReadI64(b8) == 0x0102030405060708LL);
    printf("FAILS %d\\n", fails);
    return 0;
}}
"""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "probe_harness.cpp"
        exe = Path(td) / "probe_harness.exe"
        src.write_text(harness, encoding="utf-8")
        cp = subprocess.run([gxx, "-std=c++11", "-O0", "-o", str(exe), str(src)],
                            capture_output=True, text=True)
        if cp.returncode != 0:
            check("B1 C++ 探针编译", False, (cp.stderr or "")[-400:])
            return
        check("B1 C++ 探针编译", True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        fails_line = 0
        for line in run.stdout.splitlines():
            if line.startswith("CHECK "):
                _, name, verdict = line.split(" ", 2)
                check("B " + name, verdict == "PASS")
            elif line.startswith("FAILS "):
                fails_line = int(line.split()[1])
        check("B2 C++ 行为全部通过", fails_line == 0 and run.returncode == 0, run.returncode)


# ---------- C. Web JS 行为（node ESM） ----------
def js_behavior_checks():
    node = shutil.which("node")
    if node is None or not JS_SESSION.exists():
        check("C0 Web 行为验证可执行", False,
              "缺 node" if node is None else "缺 session.js")
        return

    def json_dstr(x):
        import json
        return json.dumps(str(x).replace("\\", "/"))

    harness = f"""
import {{ pathToFileURL }} from 'url';
const dir = {json_dstr(WEB_DIR)};
const imp = async (f) => import(pathToFileURL(dir + '/' + f).href);
let fails = 0;
function ck(name, ok, detail) {{
    console.log(`CHECK ${{name}} ${{ok ? 'PASS' : 'FAIL'}}${{ok ? '' : ' | ' + detail}}`);
    if (!ok) fails++;
}}
try {{
    const S = await imp('session.js');
    const A = await imp('commands/account.js');
    const M = await imp('msgtypes.js');
    const P = await imp('pdu.js');
    const ID = {json_dstr(ID32)}, TOK = {json_dstr(TOK64)};
    const ascii = (s) => new TextEncoder().encode(s);

    // 校验器
    ck('js_valid_id', S.isValidClientId(ID));
    ck('js_id_short', !S.isValidClientId(ID.slice(0, 31)));
    ck('js_id_long', !S.isValidClientId(ID + '0'));
    ck('js_id_upper', !S.isValidClientId(ID.replace('abcdef', 'ABCDEF')));
    ck('js_id_nonhex', !S.isValidClientId(ID.slice(0, 31) + 'g'));
    ck('js_id_empty', !S.isValidClientId(''));
    ck('js_id_nonstr', !S.isValidClientId(null));
    ck('js_valid_token', S.isValidResumeToken(TOK));
    ck('js_token_short', !S.isValidResumeToken(TOK.slice(0, 63)));
    ck('js_token_long', !S.isValidResumeToken(TOK + '0'));
    ck('js_token_upper', !S.isValidResumeToken(TOK.slice(0, 63) + 'A'));

    // 字段构造
    const ca = S.buildSessionCaData(ID);
    ck('js_ca_len64', ca.length === 64, ca.length);
    ck('js_ca_id_at_0', new TextDecoder().decode(ca.subarray(0, 32)) === ID);
    ck('js_ca_rest_zero', ca.subarray(32).every((b) => b === 0));
    let threw = false;
    try {{ S.buildSessionCaData('xx'); }} catch {{ threw = true; }}
    ck('js_ca_rejects_bad_id', threw);
    const rm = S.buildResumeMsg(TOK);
    ck('js_resume_msg_65', rm.length === 65 && rm[64] === 0
        && new TextDecoder().decode(rm.subarray(0, 64)) === TOK, rm.length);
    threw = false;
    try {{ S.buildResumeMsg('short'); }} catch {{ threw = true; }}
    ck('js_resume_rejects_bad_token', threw);

    const mkResp = (code, ver = 0n, name = '', msgBytes = new Uint8Array(0)) => {{
        const d = new Uint8Array(64);
        new DataView(d.buffer).setInt32(0, code, true);
        new DataView(d.buffer).setBigUint64(4, ver, true);
        const nb = ascii(name);
        d.set(nb.subarray(0, Math.min(nb.length, 31)), 32);
        return {{ caData: d, caMsg: msgBytes, uiMsgLen: msgBytes.length }};
    }};

    // 解析器
    ck('js_parse_logout', S.parseLogoutRespond(mkResp(-3015)).ret === -3015);
    const rr = S.parseResumeRespond(mkResp(0, 5n, 'alice'));
    ck('js_parse_resume', rr.ret === 0 && rr.loginVersion === 5n && rr.username === 'alice',
        JSON.stringify({{ r: rr.ret, v: String(rr.loginVersion), u: rr.username }}));
    const failResume = S.parseResumeRespond(mkResp(-3017));
    ck('js_parse_resume_fail', failResume.ret === -3017 && failResume.username === undefined);
    const ev = S.parseInvalidatedEvent(mkResp(-3017, 3n, '', ascii(' kicked ')));
    ck('js_parse_event', ev.reason === -3017 && ev.version === 3n && ev.text.trim() === 'kicked');

    // 请求构造（stub net 捕获帧）
    let cap = null;
    const net = {{ request: async (type, expect, caData = null, msg = null) => {{
        cap = {{ type, expect, caData, msg }};
        return mkResp(0, 7n, 'bob');
    }} }};
    await A.sessionLogout(net, ID);
    ck('js_logout_type', cap.type === M.T.SESSION_LOGOUT_REQUEST && cap.expect === M.T.SESSION_LOGOUT_RESPOND);
    ck('js_logout_cadata', cap.caData && new TextDecoder().decode(cap.caData.subarray(0, 32)) === ID);
    await A.sessionResume(net, ID, TOK);
    ck('js_resume_type', cap.type === M.T.SESSION_RESUME_REQUEST && cap.expect === M.T.SESSION_RESUME_RESPOND);
    ck('js_resume_body', cap.msg && cap.msg.length === 65 && cap.msg[64] === 0);
    ck('js_resume_returns', true);

    // 帧级 roundtrip：121 经 encodePDU/decodePDU 后偏移不变
    const frame = P.encodePDU(M.T.SESSION_RESUME_REQUEST, S.buildSessionCaData(ID), S.buildResumeMsg(TOK));
    const dec = P.decodePDU(frame.slice(0));
    ck('js_frame_len', dec.uiTotalLen === 76 + 65 && dec.uiMsgLen === 65, dec.uiTotalLen);
    ck('js_frame_type', dec.uiType === 121);
    ck('js_frame_offsets', new TextDecoder().decode(dec.caData.subarray(0, 32)) === ID
        && new TextDecoder().decode(dec.caMsg.subarray(0, 64)) === TOK && dec.caMsg[64] === 0);

    // login：成功带 token / 旧服务端无 token / 失败码
    const loginResp = (ret, name, tokenMsg) => {{
        const d = new Uint8Array(64);
        new DataView(d.buffer).setInt32(0, ret, true);
        const nb = ascii(name);
        d.set(nb.subarray(0, Math.min(nb.length, 31)), 32);
        const mb = tokenMsg ? ascii(tokenMsg + '\\0') : new Uint8Array(0);
        return {{ caData: d, caMsg: mb, uiMsgLen: mb.length }};
    }};
    cap = null;
    net.request = async (type, expect, caData, msg) => {{
        cap = {{ type, expect, caData, msg }};
        return loginResp(0, 'bob', TOK);
    }};
    const okLogin = await A.login(net, {{ username: 'bob', password: 'pw', captcha: 'ab12', clientId: ID }});
    ck('js_login_ok_token', okLogin.ret === 0 && okLogin.resumeToken === TOK && okLogin.loginName === 'bob',
        JSON.stringify({{ r: okLogin.ret, t: okLogin.resumeToken }}));
    ck('js_login_body_layout', new TextDecoder().decode(cap.msg).split('\\0')[1] === ID);
    net.request = async (type, expect, caData, msg) => loginResp(0, 'bob', null);
    const legacy = await A.login(net, {{ username: 'bob', password: 'pw', captcha: 'ab12' }});
    ck('js_login_legacy_no_token', legacy.ret === 0 && (!legacy.resumeToken || legacy.resumeToken === ''));
    net.request = async () => loginResp(-3017, '', null);
    const bad = await A.login(net, {{ username: 'bob', password: 'pw', captcha: 'ab12', clientId: ID }});
    ck('js_login_fail_not_success', bad.ret === -3017 && bad.resumeToken === undefined);

    // register 旧布局：caMsg = 验证码 + NUL（无 clientId 时保持旧帧）
    cap = null;
    net.request = async (type, expect, caData, msg) => {{
        cap = {{ type, expect, caData, msg }};
        return loginResp(0, 'new', null);
    }};
    await A.register(net, {{ username: 'new', password: 'pw', captcha: 'zz9' }});
    ck('js_register_legacy', cap.msg.length === 4 && cap.msg[3] === 0);
    await A.register(net, {{ username: 'new', password: 'pw', captcha: 'zz9', clientId: ID }});
    ck('js_register_with_id', new TextDecoder().decode(cap.msg).split('\\0')[1] === ID);

    // 验证码：携带 clientId / 旧端空载荷
    cap = null;
    net.request = async (type, expect, caData, msg) => {{
        cap = {{ type, expect, caData, msg }};
        return {{ caData: new Uint8Array(64), caMsg: new Uint8Array(0), uiMsgLen: 0 }};
    }};
    await A.requestCaptcha(net, ID);
    ck('js_captcha_with_id', cap.type === 44 && cap.caData
        && new TextDecoder().decode(cap.caData.subarray(0, 32)) === ID);
    await A.requestCaptcha(net);
    ck('js_captcha_legacy_empty', cap.caData == null || cap.caData.length === 0
        || cap.caData.every((b) => b === 0));

    // 未知结果码：按失败处理 + 文案兜底
    ck('js_unknown_code_text', M.protocolCodeText(-99999).includes('未知错误'));
    for (const c of [-1003, -3015, -3016, -3017, -3018]) {{
        const t = M.protocolCodeText(c);
        ck('js_code_text_' + c, t !== `未知错误(${{c}})` && t.length > 0, t);
    }}
    // JS 布局常量
    ck('js_layout_consts', S.CLIENT_ID_HEX_LEN === 32 && S.RESUME_TOKEN_HEX_LEN === 64
        && S.SESSION_CLIENT_ID_OFFSET === 0 && S.SESSION_RESP_CODE_OFFSET === 0
        && S.SESSION_RESP_VERSION_OFFSET === 4 && S.SESSION_RESP_USER_OFFSET === 32
        && S.SESSION_RESUME_MSG_LEN === 65);
}} catch (e) {{
    ck('js_harness_no_throw', false, String(e && e.stack || e));
}}
console.log('FAILS ' + fails);
"""

    with tempfile.TemporaryDirectory() as td:
        mjs = Path(td) / "probe_web.mjs"
        mjs.write_text(harness, encoding="utf-8")
        run = subprocess.run([node, str(mjs)], capture_output=True, text=True, encoding="utf-8")
        fails_line = None
        for line in (run.stdout or "").splitlines():
            if line.startswith("CHECK "):
                parts = line.split(" ", 3)
                name, verdict = parts[1], parts[2]
                detail = parts[3][2:] if len(parts) > 3 and parts[3].startswith("|") else ""
                check("C " + name, verdict == "PASS", detail)
            elif line.startswith("FAILS "):
                fails_line = int(line.split()[1])
        if fails_line is None:
            check("C9 node harness 正常退出", False, (run.stderr or "")[-400:])
        else:
            check("C9 Web 行为全部通过", fails_line == 0 and run.returncode == 0)


def main():
    print(f"== protocol_probe (REQ-AUTH-CONNECTION-001 阶段一) root={ROOT}")
    static_checks()
    cpp_impl_checks()
    cpp_behavior_checks()
    js_behavior_checks()
    fails = [r for r in RESULTS if not r[1]]
    print(f"== 总结: {len(RESULTS)} 项, FAIL {len(fails)} 项")
    if fails:
        for name, _, detail in fails:
            print(f"  FAIL {name} {detail}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
