# build_nro.ps1 —— 在装有 devkitPro 的 Windows 上一键出 .nro
#
# 用法（PowerShell，在项目根目录）：
#   .\build_nro.ps1
#   .\build_nro.ps1 -Proxy ""            # 不走代理（能直连 GitHub 时）
#
# 前提：已安装 devkitPro pacman（默认 C:\devkitPro），并保证 pkg.devkitpro.org 可访问
#       （该站按 IP 拦 Cloudflare，国内直连常见 403，需要代理/VPN）

param(
    [string]$Proxy = "https://gh-proxy.com/"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $root

function Fail($msg) {
    Write-Host "[X] $msg" -ForegroundColor Red
    exit 1
}

# ---------- 1. 找 devkitPro ----------
$dkp = $env:DEVKITPRO
if (-not $dkp) {
    if (Test-Path -LiteralPath "C:\devkitPro") { $dkp = "C:\devkitPro" }
}
if (-not $dkp -or -not (Test-Path -LiteralPath $dkp)) {
    Fail "找不到 devkitPro。先装 devkitPro pacman，或把 DEVKITPRO 环境变量指到安装目录。"
}
$env:DEVKITPRO = $dkp
Write-Host "[+] DEVKITPRO = $dkp"

$msysBash = Join-Path $dkp "msys2\usr\bin\bash.exe"
if (-not (Test-Path -LiteralPath $msysBash)) {
    # 有些安装方式 bash 在 devkitPro\msys2 下，有些直接用系统 msys2
    $msysBash = Join-Path $dkp "usr\bin\bash.exe"
    if (-not (Test-Path -LiteralPath $msysBash)) {
        Fail "找不到 devkitPro 自带的 bash（试过 $dkp\msys2\usr\bin\bash.exe 和 $dkp\usr\bin\bash.exe）"
    }
}

$msysRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $msysBash))
$dkpPacman = Join-Path $msysRoot "usr\bin\bash.exe"
# devkitPro 的 dkp-pacman 是一个包装脚本，统一通过 bash -lc 调
$bashCmd = $msysBash

function ToMsysPath($p) {
    $full = (Resolve-Path -LiteralPath $p).Path
    return "/" + ($full -replace '^([A-Za-z]):\\', '$1/').Replace('\', '/')
}

# ---------- 2. 装依赖 ----------
Write-Host "[*] 检查 pacman 依赖（switch-dev / glfw / curl / libmpv / cmake）…"
& $bashCmd -lc "dkp-pacman -S --needed --noconfirm switch-dev switch-glfw switch-curl switch-libmpv switch-cmake"
if ($LASTEXITCODE -ne 0) {
    Fail "pacman 安装失败。若报 403 / 连接失败，说明 pkg.devkitpro.org 被拦，请开代理后重试。"
}

# ---------- 3. 拉 borealis ----------
if (-not (Test-Path -LiteralPath (Join-Path $root "libs\borealis\CMakeLists.txt"))) {
    Write-Host "[*] 拉取 borealis…"
    New-Item -ItemType Directory -Force -Path (Join-Path $root "libs") | Out-Null
    $url = if ($Proxy) { "$Proxy" + "https://github.com/XITRIX/borealis.git" } else { "https://github.com/XITRIX/borealis.git" }
    & $bashCmd -lc "cd '$(ToMsysPath $root)' && git clone --depth 1 '$url' libs/borealis"
    if ($LASTEXITCODE -ne 0) { Fail "borealis 克隆失败，检查网络或 -Proxy 参数" }
}

# ---------- 4. 编译 ----------
Write-Host "[*] cmake 配置 + 编译…"
$msysRoot = ToMsysPath $root
& $bashCmd -lc "cd '$msysRoot' && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4"
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "[!] 编译失败。常见原因见 README 第七节：" -ForegroundColor Yellow
    Write-Host "    1) View::draw 形参版本不一致" -ForegroundColor Yellow
    Write-Host "    2) mpv 头文件路径（switch-libmpv 未装或版本不同）" -ForegroundColor Yellow
    Write-Host "    3) nx_generate_nacp / nx_create_nro 参数名与 devkitPro 版本不符" -ForegroundColor Yellow
    exit 1
}

# ---------- 5. 收产物 ----------
$nro = Get-ChildItem -Path (Join-Path $root "build") -Filter *.nro -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $nro) {
    Fail "编译跑完了但没找到 .nro，检查 CMakeLists.txt 里 nx_create_nro 段"
}

$dest = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -LiteralPath $nro.FullName -Destination (Join-Path $dest "tvbox-nx.nro") -Force

Write-Host ""
Write-Host "[OK] 产物：$(Join-Path $dest 'tvbox-nx.nro')" -ForegroundColor Green
Write-Host "     装到 Switch：sd:/switch/tvbox-nx/tvbox-nx.nro"
Write-Host "     想要 HOME 图标：sphaira → Install Forwarder → 指向这个 nro"
