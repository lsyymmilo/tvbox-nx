# 上传到 GitHub，用 Actions 编出 .nro（6 步）

前提：解压 `tvbox-nx.zip`，得到 `tvbox-nx` 文件夹。确认里面能看到 `.github` 这个目录（Windows 不会隐藏它）。

---

## 第 1 步：建仓库

1. 打开 <https://github.com/new>
2. **Repository name** 填 `tvbox-nx`
3. 可见性选 **Public**
4. **不要**勾 "Add a README file"（多一个 commit 只会让上传步骤变复杂）
5. 点 **Create repository**

## 第 2 步：上传全部文件

1. 新仓库页面点 **uploading an existing file**（或 Add file → Upload files）
2. 把 `tvbox-nx` 文件夹里的**所有内容**一起拖进去，包括这三个容易漏的：
   - `.github`（里面是 `workflows/build-nro.yml`，**漏了它就不会自动构建**）
   - `source`
   - `tools`
3. 拖完先看页面左侧文件清单，必须能看到 `.github/workflows/build-nro.yml`
4. 底部点 **Commit changes**

## 第 3 步：等 Actions 开始跑

1. 点仓库顶部 **Actions** 标签
2. 左侧选 **build-nro**
3. 正常情况下这次 push 会自动触发，1 分钟内在列表里出现一条新记录
4. 没自动跑就点右侧 **Run workflow** → 分支选 `main` → **Run workflow**
5. 首次构建要拉 devkitPro 镜像和依赖，**大约 3~6 分钟**

如果 Actions 页面提示 “Workflows aren't being run”，去 **Settings → Actions → General**，把
**Allow all actions and reusable workflows** 选上再重跑。

## 第 4 步：下载 .nro

1. 点进那条运行记录（绿色对勾表示成功）
2. 页面拉到最下面 **Artifacts** 区域
3. 点 **tvbox-nx-nro** 下载
4. 解压下载的 zip，里面是 **`tvbox-nx.nro`** —— 这就是成品

## 第 5 步：装到 Switch

```
sd:/switch/tvbox-nx/tvbox-nx.nro     ← 主程序
sd:/switch/tvbox-nx/config.json      ← 你的源配置（可选，程序里有内置示例兜底）
```

想要 HOME 菜单图标：Switch 上打开 **sphaira** → **Install Forwarder** → 选中这个 `.nro`。

## 第 6 步：如果构建红了

1. 点进红色运行记录
2. 左侧步骤列表里点红色的那一步
3. 右上角齿轮菜单 → **View raw log**（或 Ctrl+A 全选复制）
4. 把那段贴给我，我按行改代码，你重新上传改动的文件再跑一次

**我最预判的三个报错**，贴日志时注意看是不是这些：

| 报错关键字 | 原因 | 我要改的地方 |
|---|---|---|
| `draw` / `no matching function` / `NVGcontext` | `View::draw` 形参跟你的 borealis 版本不一致 | `source/ui.cpp` 的 `MpvView::draw` |
| `mpv/client.h: No such file` | switch-libmpv 没进容器或版本布局不同 | workflow 的 pacman 行 |
| `nx_create_nro` / `nx_generate_nacp` 参数 | devkitPro cmake 模块版本差异 | `CMakeLists.txt` 末尾两段 |

---

## 附：关于公开仓库

你选了 Public，意味着源码对所有人可见（本项目本身不含任何内容源，只有播放器代码和一个示例配置）。
想改回来：**Settings → 最底部 Danger Zone → Change repository visibility → Private**。
