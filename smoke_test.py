
import socket
import struct
import time

# PDU struct: uint(4) uiTotalLen, uint(4) uiMsgLen, uint(4) uiType, char[64] caData, char[] caMsg
PDU_HEADER_SIZE = 4 + 4 + 4 + 64

def mk_pdu(msg_type, data_bytes=b'', msg_bytes=b''):
    msg_len = len(msg_bytes)
    total_len = PDU_HEADER_SIZE + msg_len
    caData = data_bytes.ljust(64, b'\x00')
    return struct.pack('III64s', total_len, msg_len, msg_type, caData) + msg_bytes

def test_connection():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2)
        s.connect(('127.0.0.1', 5000)) # 修正端口为 5000
        print("Connected to server.")
        
        # Test Heartbeat (Type 50)
        pdu = mk_pdu(50)
        s.sendall(pdu)
        resp = s.recv(PDU_HEADER_SIZE)
        total_len, msg_len, ui_type, caData = struct.unpack('III64s', resp)
        print(f"Heartbeat Resp: Type={ui_type} (Expected 51)")
        
        # Test Share Link (Type 52)
        pdu = mk_pdu(52, data_bytes=b"/test/path/file.txt")
        s.sendall(pdu)
        resp = s.recv(PDU_HEADER_SIZE)
        total_len, msg_len, ui_type, caData = struct.unpack('III64s', resp)
        ok = struct.unpack('?', caData[:1])[0]
        msg = s.recv(msg_len) if msg_len > 0 else b''
        print(f"Share Link Resp: Type={ui_type} (Expected 53), OK={ok}, Code={msg.decode('utf-8') if ok else 'N/A'}")
        
        s.close()
    except Exception as e:
        print(f"Server not running or error: {e}")

if __name__ == '__main__':
    test_connection()
