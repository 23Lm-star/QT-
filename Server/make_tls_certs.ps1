# REQ-EXT-TLS-001 自签证书生成（部署机一次性执行）
# 产出 dist/certs/server.crt 与 server.key（CN=tlsserver，SAN=IP:127.0.0.1，825 天）。
# 私钥只留在运行目录；不要提交到 Git、不要粘贴到日志或文档。
# 用法（项目根，PowerShell）：
#   .\Server\make_tls_certs.ps1
param(
    [string]$OutDir = "",
    [int]$Days = 825
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($OutDir)) { $OutDir = Join-Path $root 'dist\certs' }

$ossl = (Get-Command openssl.exe -ErrorAction SilentlyContinue)
if (-not $ossl) { throw '未找到 openssl.exe：请安装 OpenSSL 命令行工具后重试（Qt 运行时还需要 libssl-1_1-x64.dll/libcrypto-1_1-x64.dll，见 README）' }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$crt = Join-Path $OutDir 'server.crt'
$key = Join-Path $OutDir 'server.key'
if (Test-Path $crt) { throw "$crt 已存在，不覆盖。确要重签请先手动移除旧证书与私钥。" }

& openssl.exe req -x509 -newkey rsa:2048 -keyout $key -out $crt -days $Days -nodes `
    -subj '/CN=tlsserver' -addext 'subjectAltName=IP:127.0.0.1'
if ($LASTEXITCODE -ne 0) { throw "openssl req 失败，退出码 $LASTEXITCODE" }

Write-Output "已生成：$crt"
Write-Output "客户端 pinning 用同一份 server.crt（复制到 Client 资源后重新构建）。"
