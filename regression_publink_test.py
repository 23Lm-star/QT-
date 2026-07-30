import socket
import struct
import time
import hashlib

# PDU 结构: uint(4) uiTotalLen, uint(4) uiMsgLen, uint(4) uiType, char[64] caData, char[] caMsg
PDU_HEADER_SIZE = 4 + 4 + 4 + 64

def mk_pdu(msg_type, data_bytes=b'', msg_bytes=b''):
    msg_len = len(msg_bytes)
    total_len = PDU_HEADER_SIZE + msg_len
    caData = data_bytes.ljust(64, b'\x00')
    return struct.pack('III64s', total_len, msg_len, msg_type, caData) + msg_bytes

def parse_pdu(data):
    if len(data) < PDU_HEADER_SIZE:
        return None
    total_len, msg_len, ui_type, caData = struct.unpack('III64s', data[:PDU_HEADER_SIZE])
    msg = data[PDU_HEADER_SIZE:PDU_HEADER_SIZE + msg_len]
    return total_len, msg_len, ui_type, caData, msg

def run_regression_test():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(('127.0.0.1', 5000))
        print("[1] 已连接到服务器")

        # 0. 请求验证码 (Type 44)
        captcha_pdu = mk_pdu(44)
        s.sendall(captcha_pdu)
        resp = s.recv(1024)
        _, _, c_resp_type, _, c_resp_msg = parse_pdu(resp)
        captcha_code = c_resp_msg.split(b'\x00')[0]
        print(f"[0] 获取验证码成功: {captcha_code.decode()}")

        # 1. 登录（由于需要真实用户，我尝试用之前测试报告中提到的 wcy/123456）
        # 密码是 MD5
        pwd_raw = b"123456"
        pwd_md5 = hashlib.md5(pwd_raw).hexdigest().encode()
        
        # 注册一个新用户以确保环境干净且知道密码
        user_id = int(time.time()) % 10000
        test_user_str = f"test{user_id}"
        test_user = test_user_str.encode()
        pwd_md5_str = hashlib.md5(b"123456").hexdigest().encode()
        
        # 注册请求 caData：用户名(32) + 密码MD5(32)
        reg_ca_data = test_user.ljust(32, b'\x00') + pwd_md5_str.ljust(32, b'\x00')
        reg_pdu = mk_pdu(1, data_bytes=reg_ca_data, msg_bytes=captcha_code)
        s.sendall(reg_pdu)
        time.sleep(1)
        reg_resp = s.recv(1024) # 等待注册结果
        print(f"[+] 注册用户 {test_user_str} 发送完毕")
        
        # 强制下线 wcy
        off_pdu = mk_pdu(4) # 假设 4 是登录？不，3是登录。
        # 我们可以通过重新启动服务器来重置在线状态，或者假设 wcy 已经在线
        
        # 重新尝试 wcy 登录，这次使用 md5 后的 123456
        # 注意：数据库存的是 md5(123456)
        # 检查注册响应
        reg_ret = struct.unpack('?', parse_pdu(reg_resp)[3][:1])[0]
        if not reg_ret:
            print("[-] 注册失败，可能用户已存在")

        # 获取登录用的新验证码
        s.sendall(mk_pdu(44))
        captcha_code = parse_pdu(s.recv(1024))[4].split(b'\x00')[0]
        print(f"[+] 获取登录验证码: {captcha_code.decode()}")
        
        # 登录
        # 尝试使用硬编码用户（如果注册一直失败）
        test_user = b"wcy"
        test_user_str = "wcy"
        # 重新生成 caData 和验证码
        s.sendall(mk_pdu(44))
        captcha_code = parse_pdu(s.recv(1024))[4].split(b'\x00')[0]
        
        login_ca_data = test_user.ljust(32, b'\x00') + pwd_md5_str.ljust(32, b'\x00')
        login_pdu = mk_pdu(3, data_bytes=login_ca_data, msg_bytes=captcha_code)
        s.sendall(login_pdu)
        resp = s.recv(1024)
        _, _, l_type, l_data, _ = parse_pdu(resp)
        ret = struct.unpack('i', l_data[:4])[0]
        if ret != 1:
            print(f"[-] 登录失败: ret={ret}.")
            return

        print(f"[+] 登录成功: {test_user_str}")

        # 2. 创建分享链接 (Type 52)
        # 修复后的代码期望 caMsg 传相对路径
        path = b"test.txt"
        create_pdu = mk_pdu(52, msg_bytes=path)
        s.sendall(create_pdu)
        resp = s.recv(1024)
        _, m_len, c_type, c_data, c_msg = parse_pdu(resp)
        ok = struct.unpack('?', c_data[:1])[0]
        link = c_msg.decode('utf-8') if ok else "N/A"
        print(f"[2] 创建链接结果: OK={ok}, Link={link}")
        
        if not ok:
            print("[-] 创建链接失败，停止后续测试")
            return
        
        token = link.split("/")[-1]

        # 3. 查询分享链接 (Type 56)
        query_pdu = mk_pdu(56, data_bytes=test_user, msg_bytes=path)
        s.sendall(query_pdu)
        resp = s.recv(1024)
        _, _, q_type, q_data, q_msg = parse_pdu(resp)
        q_ok = struct.unpack('?', q_data[:1])[0]
        q_link = q_msg.decode('utf-8') if q_ok else "N/A"
        print(f"[3] 查询链接结果: OK={q_ok}, Link={q_link}")
        
        # TODO: 验证 128-bit 随机性（通过 token 长度和字符集初步推断）
        print(f"[*] Token 长度检查: {len(token)} 字节")

        # 4. 解析分享链接 (Type 54)
        resolve_pdu = mk_pdu(54, msg_bytes=token.encode())
        s.sendall(resolve_pdu)
        resp = s.recv(1024)
        _, _, r_type, r_data, r_msg = parse_pdu(resp)
        r_ok = struct.unpack('?', r_data[:1])[0]
        r_info = r_msg.decode('utf-8') if r_ok else "N/A"
        print(f"[4] 解析链接结果: OK={r_ok}, Info={r_info}")

        # 5. 用户注销 (Type 48) - 验证清理
        logoff_ca = test_user.ljust(32, b'\x00') + pwd_md5.ljust(32, b'\x00')
        logoff_pdu = mk_pdu(48, data_bytes=logoff_ca)
        s.sendall(logoff_pdu)
        s.recv(1024)
        print(f"[5] 用户 {test_user_str} 已注销")
        
        # 再次尝试解析（应失败，因为记录被清理）
        s.sendall(resolve_pdu)
        r_ok_after = struct.unpack('?', parse_pdu(s.recv(1024))[3][:1])[0]
        print(f"[6] 注销后解析结果: OK={r_ok_after} (预期 False)")

        s.close()
    except Exception as e:
        print(f"[-] 测试异常: {e}")

if __name__ == '__main__':
    run_regression_test()
