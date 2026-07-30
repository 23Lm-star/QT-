
import sqlite3
import hashlib
import secrets
import datetime
import base64

def run_tier2_simulation():
    print("=== Tier-2 Python Simulation: Share Link Logic & Cleanup ===")
    
    # 1. Setup Mock DB (SQLite instead of MySQL)
    conn = sqlite3.connect(':memory:')
    cursor = conn.cursor()
    
    cursor.execute("""
    CREATE TABLE user_info (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name VARCHAR(255),
        pwd VARCHAR(255),
        online INTEGER DEFAULT 0
    )""")
    
    cursor.execute("""
    CREATE TABLE share_link (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        token_hash CHAR(64) NOT NULL UNIQUE,
        owner_name VARCHAR(255) NOT NULL,
        relative_path VARCHAR(1024) NOT NULL,
        expire_time DATETIME NOT NULL,
        revoked TINYINT NOT NULL DEFAULT 0,
        created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
    )""")
    
    cursor.execute("""
    CREATE TABLE publish_links (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        short_code VARCHAR(32) UNIQUE NOT NULL,
        file_path VARCHAR(255) NOT NULL,
        creator_name VARCHAR(32) NOT NULL,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    )""")

    # 2. Simulate Token Generation (128-bit)
    def generate_token():
        # Mirroring QByteArray::toBase64(Base64UrlEncoding | OmitTrailingEquals)
        data = secrets.token_bytes(16)
        return base64.urlsafe_b64encode(data).decode().rstrip('=')

    def hash_token(token):
        return hashlib.sha256(token.encode()).hexdigest()

    # 3. Test Creation
    test_user = "alice"
    token = generate_token()
    token_hash = hash_token(token)
    path = "docs/secret.txt"
    expire = (datetime.datetime.now() + datetime.timedelta(days=1)).strftime('%Y-%m-%d %H:%M:%S')
    
    print(f"[+] 生成 Token: {token} (长度: {len(token)})")
    print(f"[+] Token Hash: {token_hash}")
    
    cursor.execute("INSERT INTO share_link (token_hash, owner_name, relative_path, expire_time) VALUES (?, ?, ?, ?)",
                   (token_hash, test_user, path, expire))
    
    # Simulate old publish_links entry
    cursor.execute("INSERT INTO publish_links (short_code, file_path, creator_name) VALUES (?, ?, ?)",
                   ("OLD123", "old/path.txt", test_user))
    
    conn.commit()
    print("[+] 记录已插入 share_link 和 publish_links")

    # 4. Test Resolve Logic
    def resolve(token_input):
        h = hash_token(token_input)
        cursor.execute("SELECT owner_name, relative_path, expire_time, revoked FROM share_link WHERE token_hash = ?", (h,))
        row = cursor.fetchone()
        if not row: return None, "Not Found"
        owner, rel_path, exp, rev = row
        if rev: return None, "Revoked"
        if datetime.datetime.strptime(exp, '%Y-%m-%d %H:%M:%S') < datetime.datetime.now():
            return None, "Expired"
        return rel_path, "OK"

    res_path, status = resolve(token)
    print(f"[+] 解析 Token '{token}': {status}, Path={res_path}")
    
    # 5. Test Expiry
    past_expire = (datetime.datetime.now() - datetime.timedelta(hours=1)).strftime('%Y-%m-%d %H:%M:%S')
    cursor.execute("UPDATE share_link SET expire_time = ? WHERE token_hash = ?", (past_expire, token_hash))
    _, status_exp = resolve(token)
    print(f"[+] 过期 Token 验证: {status_exp} (预期 Expired)")

    # 6. Test Cleanup on Logout (Logoff)
    print(f"[!] 模拟用户 {test_user} 注销清理...")
    cursor.execute("DELETE FROM share_link WHERE owner_name = ?", (test_user,))
    cursor.execute("DELETE FROM publish_links WHERE creator_name = ?", (test_user,))
    conn.commit()
    
    cursor.execute("SELECT COUNT(*) FROM share_link WHERE owner_name = ?", (test_user,))
    count1 = cursor.fetchone()[0]
    cursor.execute("SELECT COUNT(*) FROM publish_links WHERE creator_name = ?", (test_user,))
    count2 = cursor.fetchone()[0]
    
    print(f"[+] 清理结果: share_link 剩余={count1}, publish_links 剩余={count2}")
    if count1 == 0 and count2 == 0:
        print("[SUCCESS] 级联清理逻辑验证通过")
    else:
        print("[FAILURE] 清理逻辑有误")

if __name__ == '__main__':
    run_tier2_simulation()
