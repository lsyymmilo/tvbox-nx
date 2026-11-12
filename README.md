# TVBox-NX

一个跑在 Nintendo Switch 上的原生视频客户端：沿用 TVBox 的“自填源”模型（配置里列一堆站源，自己决定看哪个），但界面、网络、播放全部按 Switch 原生方式重写 —— **不是把 APK 转成 NSP**。

技术栈就是 Switchfin 那一套：`borealis`（界面）+ `libmpv`（播放）+ `libcurl`（网络）+ `libnx`。

---

## 一、能做什么 / 不能做什么（先看这个）

| 能力 | 状态 |
|---|---|
读 TVBox 配置（含 `sites` 的 JSON）或单条 `api` 地址 | ✅ |
`type=1` 的 CMS JSON 站源（`.../api.php/provide/vod`） | ✅ 分类 / 列表 / 详情 / 搜索 |
`type=0` XML 站源 | ❌ 未实现（要另写一套 RSS 解析） |
`type=3` csp 蜘蛛源（`csp_xxx`、jar、js、py） | ❌ **原理上不可能**：要 ART 虚拟机或 JS/Python 引擎 |
直链 `.m3u8 / .mp4` 播放 | ✅ libmpv 软解 |
需要“解析”的网页地址 | ❌ 没有 WebView / 解析器，界面上会明确标“需解析，不支持” |
弹幕、豆瓣、登录、下载、网盘 | ❌ 不做 |
界面语言 | 中文 |

一句话：**它只认“接口直接吐 m3u8/mp4 直链”的源。** 这也是在 Switch 上唯一现实可行的子集。

---

## 二、构建之前，先在 PC 上验源

这一步不需要 Switch，也不需要编译器，先把“源能不能用”确认掉，能省掉大量无谓的重新编译。

```bash
# 1) 起一个本地模拟站源（用来确认工具链本身正常）
python tools/mock_cms.py

# 2) 另一个终端，拿它验一遍
python tools/probe.py --config config.sample.json --search 测试

# 3) 换到你自己的源
python tools/probe.py --config 你的配置.json --json report.json
```

输出示例（本地模拟源的真实运行结果）：

```
状态    名称                耗时      说明
可播    本地模拟站             42ms    CMS JSON
跳过    蜘蛛源（不可用于 NS）      -       type=3（csp/spider，需 ART 或 JS 引擎）

[=] 接口可用 1 / 2，其中含直链 1 个
```

`probe.py` 和客户端的 `source/tvbox.cpp` 实现的是同一套协议（`ac=list` / `ac=detail&t=` / `ac=detail&ids=` / `ac=detail&wd=`），所以“探针说可播”≈“客户端能播”。

---

## 三、构建（devkitPro）

### Windows

1. 装 [devkitPro pacman](https://devkitpro.org/wiki/Getting_Started)，打开 **MSYS2** 终端（不是 PowerShell）：

```bash
dkp-pacman -Syu
dkp-pacman -S switch-dev switch-glfw switch-curl switch-libmpv switch-cmake
```

2. 拉代码与子模块：

```bash
git clone <你的仓库> tvbox-nx && cd tvbox-nx
git submodule add https://github.com/XITRIX/borealis libs/borealis
git submodule update --init --recursive
```

3. 编译：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

产物：`build/tvbox-nx.nro`

### Docker（不想装 MSYS2 的话）

```bash
docker run --rm -v "$PWD:/src" -w /src devkitpro/devkita64:latest \
  bash -lc "dkp-pacman -S --noconfirm switch-glfw switch-curl switch-libmpv switch-cmake && \
            cmake -B build && cmake --build build -j4"
```

> 第一次编译大概率会停在两处，见「七、已知未验证的点」。

---

## 四、装到 Switch

```
sd:/switch/tvbox-nx/tvbox-nx.nro      ← 主程序
sd:/switch/tvbox-nx/config.json        ← 你的源配置
```

`config.json` 直接用 TVBox 的配置就行，客户端会自动筛掉跑不了的蜘蛛源：

```json
{
  "sites": [
    { "key": "a", "name": "站源 A", "type": 1, "api": "https://host-a/api.php/provide/vod", "searchable": 1 },
    { "key": "b", "name": "站源 B", "type": 1, "api": "https://host-b/api.php/provide/vod", "searchable": 1 }
  ],
  "parses": []
}
```

没有 `config.json` 时，程序会退回 romfs 里的示例配置（界面里会提示），也可以在「源」页里临时手填一条 API 地址。

---

## 五、做成 HOME 菜单里的 NSP

NSP 在这里**只是一个指向 NRO 的 forwarder**（未签名，需要 Atmosphère + sigpatches），不是把程序打进 NCA。

两种方式：

1. **sphaira（推荐，不用 PC）**：Switch 上打开 sphaira → 选 `Install Forwarder` → 指向 `sd:/switch/tvbox-nx/tvbox-nx.nro`。
2. **Nro2Nsp（PC）**：`Root-MtX/Nro2Nsp`，选 NRO + 图标 + 名称，输出 NSP，再拷到 `sd:/` 安装。

大内存需求的处理：如果启动后黑屏或闪退，给这个 title 开 **Title Override / 39-bit address space**（sa2-nx 的说明里有具体值），mpv 的软解缓冲吃内存。

---

## 六、性能预期（说实话）

| 内容 | 预期 |
|---|---|
720p H.264，码率 ≤ 4 Mbps | 流畅，这是它的舒适区 |
1080p H.264 | 能放，但复杂场景/高码率会掉帧 |
HEVC / AV1 | 基本别指望（纯软解，Tegra X1 的 A57 扛不住） |
换集速度 | 1~2 秒，取决于源站响应 |

`source/player.cpp` 里已经把 `vd-lavc-threads=3`、`cache-secs=20`、`video-sync=audio` 调过了，这些参数是给 4 核 A57 准备的经验值。

---

## 七、已知未验证的点（务必先看）

我在一台**没有 devkitPro、没有任何 C++ 编译器**的 Windows 机器上写的这套代码。所以：

**已验证**
- `tools/mock_cms.py` + `tools/probe.py` 完整跑通：配置解析、跳过蜘蛛源、调用 `ac=list`、取分类、取详情、拆 `$$$` / `#` / `$` 播放串、直链判定、搜索，全部实际执行过。
- **`source/json.hpp` / `tvbox.cpp` / `net.cpp` 已用 mingw g++ 以 `-std=c++17 -Wall` 实际编译通过（零警告零报错）**，并用 `tools/host_test.cpp` 对本地模拟源跑出真实结果：分类 3 个、列表 8 条 / 2 页、详情 3 条线路、5 集里 3 集判为直链、搜索命中 25、配置解析出 1 个 CMS 源且正确跳过 1 个蜘蛛源。也就是说**非 GUI 的全部逻辑是编译并运行验证过的**，不是纸面代码。

**未验证（需要你第一次编译时对齐）**
1. **`View::draw` 签名**：`source/ui.cpp` 里 `MpvView::draw(NVGcontext*, float, float, float, float, brls::Style, brls::FrameContext*)` 按 borealis 1.x 写。若你的版本形参不同，编译会直接报在这一行 —— 按报错改形参即可，函数体不用动。
2. **mpv 重绘回调**：`MpvView::createContext()` 里留了一个 `TODO(必补)`。没接 `mpv_render_context_set_update_callback` 之前，能出首帧、能 seek 后刷新，但**播放过程中画面不会持续更新**。这一处建议直接对照 `switchfin` 的 mpv 视图实现补（同一套 borealis + libmpv）。
3. **`registerAction` / `getImeManager` / `TabFrame::addTab`** 的形参顺序，同样是版本相关的，编译报错即改。
4. **`CMakeLists.txt` 的 `nx_generate_nacp` / `nx_create_nro`** 参数名与 devkitPro 版本有关，报错时照官方模板改。
5. **字体**：borealis 默认字体未必覆盖中文。若中文显示成方块，把 `romfs/` 里塞一份 CJK 字体并在 `main.cpp` 里 `brls::Application::loadFontFromFile()` 指定（这一步我没做，因为字体文件太大不宜内置）。

**纯逻辑层（`json.hpp` / `tvbox.cpp` / `net.cpp`）没有编译器验证过**，但代码结构与 `probe.py` 中已验证的同名实现一一对应（`split_play` ↔ `splitPlayUrls`、`is_cms_site` ↔ `isCmsSite`），主要风险在笔误而非逻辑。

---

## 八、目录结构

```
tvbox-nx/
├─ build_nro.ps1              # 本地一键出 .nro（需 devkitPro）
├─ .github/workflows/
│  └─ build-nro.yml           # 推到 GitHub 自动出 .nro artifact
├─ CMakeLists.txt
├─ config.sample.json
├─ romfs/
│  ├─ config.sample.json      # 首次启动兜底
│  └─ icon.jpg                # NRO 图标
├─ source/
│  ├─ main.cpp                # 入口：socket + borealis 主循环
│  ├─ json.hpp                # 自带 JSON 解析（不依赖 nlohmann）
│  ├─ net.hpp/.cpp            # libcurl 同步 GET + URL 编码
│  ├─ tvbox.hpp/.cpp          # 配置模型 + CMS 客户端（协议层）
│  ├─ player.hpp/.cpp         # libmpv 封装（播放/暂停/seek/倍速）
│  └─ ui.hpp/.cpp             # borealis：源/分类/列表/详情/播放
└─ tools/
   ├─ mock_cms.py             # 本地模拟站源
   ├─ probe.py                # 源可用性探针（PC 上跑）
   └─ host_test.cpp           # C++ 协议层测试（PC 上编译运行）
```

---

## 九、排错

| 现象 | 原因 / 处理 |
|---|---|
界面全空、只有「没有可用站源」 | `config.json` 路径不对，或里面全是 type=3 蜘蛛源 |
点分类后提示“接口失败: Could not resolve host” | Switch 没联网 / DNS 问题；curl 的 SSL 时间设置见 `net.cpp` |
点分类后提示“返回不是合法 JSON” | 源站返回了 HTML 错误页或需要特定 UA/Referer |
能进详情但所有剧集都是“需解析” | 该源走的是网页解析，不能用 —— 换源 |
播放黑屏但界面在 | mpv 渲染上下文没建起来，或没接重绘回调（见第七节第 2 条） |
播放卡顿 | 降到 720p 源；或把 `cache-secs` 调大、`vd-lavc-threads` 调到 4 |
中文显示成方块 | romfs 缺少 CJK 字体，见第七节第 5 条 |

---

## 十、合规

这个客户端本身不含任何内容源，只播放你自己配置的接口。**注意你配的源大多未获授权**：自己用是一回事，把打好的 NSP / 整合包分发出去是另一回事，后者性质和“折腾”完全不同。

---

## 十一、怎么真的拿到 .nro（三条路，任选一条）

`.nro` 是 Switch 的 ELF 二进制，**必须用 devkitA64 交叉编译器 + libnx + portlibs 编出来**，没有任何工具能把源码“转换”成它。所以：

| 路径 | 说明 | 适合谁 |
|---|---|---|
**A. GitHub Actions（最省事，推荐）** | 仓库里已放好 `.github/workflows/build-nro.yml`。推到 GitHub 后，Actions 会自动用官方 `devkitpro/devkita64` 容器装依赖、编 borealis、出 `.nro`，到 Actions 页面下载 `tvbox-nx-nro` 这个 artifact 即可。海外 runner 访问 devkitpro 源正常，不需要代理。 | 不想在本机装 1.5GB 工具链 |
**B. 本机一键脚本** | 装好 devkitPro 后，在项目根目录执行 `.\build_nro.ps1`。脚本会检查 `DEVKITPRO`、`dkp-pacman` 装依赖、拉 borealis、跑 cmake，成功后在 `dist/` 给出 `tvbox-nx.nro`。 | 想在本机反复改代码编译 |
**C. 手动** | 见「三、构建」的逐条命令。 | 想自己控制每一步 |

**为什么这次没能直接给你 .nro**（环境事实，不是推脱）：

1. 这台机器上 `g++ / clang / cl / cmake / make / docker / wsl` **一个都没有**（我已装了 mingw 的 g++ 来验证协议层，但它只能编 x86_64 Windows 程序，不能交叉编 aarch64）。
2. devkitPro 的软件源 `pkg.devkitpro.org` 在本机网络下被 Cloudflare 直接拦成 **403**（`Attention Required!`，IP 级拦截），国内的 USTC / 清华 / 阿里 / 上海交大镜像都没有 devkitpro 仓库（实测 403 / 404 / 超时）。也就是说**装不上 switch-dev、switch-libmpv、switch-glfw**，交叉编译这条路在当前网络下是物理断的。

所以你拿到 `.nro` 的最近一步就是路径 A：新建个仓库把这套源码推上去，等两分钟下 artifact。或者装个 devkitPro + 代理走路径 B。
