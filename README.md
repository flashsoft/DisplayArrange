# DisplayArrange

> macOS 菜单栏小工具：锁屏/睡眠后自动恢复双屏排列，不再每次手动拖。

**平台要求：macOS 13+，Apple Silicon 和 Intel 均支持（universal binary）。**

[English](#english) | 中文

---

## 它解决什么问题

macOS 在锁屏、睡眠或显示器重新握手后，经常把多显示器的相对位置重置成默认的"左右并排"。这是因为系统按 `CGDirectDisplayID` 记忆布局，而这个 ID 每次唤醒都可能重新分配，导致"认不出"原来的屏幕组合。

DisplayArrange 用 **display unit**（绑定物理端口的稳定标识）而不是会漂移的 display ID 来识别显示器，所以唤醒后 ID 重排也能准确还原你的布局。

## 安装

### 方式一：下载 App（推荐）

从 [Releases](https://github.com/flashsoft/DisplayArrange/releases) 下载 `DisplayArrange.app.zip`，解压后拖进「应用程序」文件夹即可。只要命令行的话可以下 `DisplayArrange-macos-universal.zip`。

首次打开如果提示"未受信任的开发者"，在 系统设置 → 隐私与安全性 里点"仍要打开"。

### 方式二：自己编译

需要 macOS 13+ 和 Xcode Command Line Tools。

```sh
git clone https://github.com/flashsoft/DisplayArrange.git
cd DisplayArrange
make app
open dist/DisplayArrange.app
```

编译产物在 `dist/` 目录下（`dist/DisplayArrange.app` 和 `dist/displayarrange`），app 可以拖到「应用程序」或任何位置。

## 使用

启动后菜单栏会出现一个双屏图标，点开就是控制面板：

| 功能 | 说明 |
|------|------|
| **状态栏** | 显示当前连接了几台显示器、是否已保存布局 |
| **保存当前布局** | 把现在的排列记下来（快捷键 `⌘S`） |
| **立即恢复布局** | 马上摆回保存的位置（快捷键 `⌘R`） |
| **解锁后自动恢复** | 开关。打开后每次解锁都会自动摆正 |
| **延迟滑块** | 解锁后等 0–5 秒再恢复，给显示器重新握手留时间 |
| **开机自动启动** | 登录后自动运行，不用手动开 |
| **退出** | 完全退出 |

### 推荐工作流

1. 在 系统设置 → 显示器 里把双屏摆成你想要的样子
2. 点菜单栏图标 → **保存当前布局**
3. 打开 **解锁后自动恢复** 和 **开机自动启动**
4. 以后每次锁屏解锁，布局都会在你设的延迟后自动还原

## 命令行版本

除了图形 app，仓库还提供一个功能相同的 CLI，适合脚本化或远程 SSH 使用。

```sh
make cli                    # 编译出 dist/displayarrange
./dist/displayarrange list                    # 查看当前布局
./dist/displayarrange save                    # 保存布局
./dist/displayarrange apply                   # 立即恢复
./dist/displayarrange watch --debounce 1000   # 后台监听解锁事件
```

CLI 和 app 共用同一份布局文件 `~/.config/displayarrange/layout.json`，保存的配置互通。

## 工作原理

1. **枚举**：用 `CGGetOnlineDisplayList`（不是 active list），睡着的显示器也能看到
2. **身份**：主键是 `CGDisplayUnitNumber`（物理端口稳定 ID），vendor/model/serial 做兜底
3. **布局**：JSON 存每台显示器的 unit、原点坐标、分辨率
4. **应用**：走 `CGBeginDisplayConfiguration` + `CGConfigureDisplayOrigin` + `CGCompleteDisplayConfiguration`，和系统自己用的是同一套 API
5. **监听**：订阅 `com.apple.screenIsUnlocked` 分布式通知，按用户设置的延迟触发恢复

核心逻辑在 [DisplayCore.c](core/DisplayCore.c)（纯 C，约 300 行），CLI 和 app 都只是它的薄壳。

## 和 jakehilborn/displayplacer 的区别

| 维度 | displayplacer | DisplayArrange |
|------|---------------|----------------|
| 显示器识别 | `CGDirectDisplayID`（会漂移） | `CGDisplayUnitNumber`（端口级稳定） |
| 自动恢复 | 无，需配合第三方工具 | 内置监听解锁事件，原生支持 |
| 用户界面 | 纯命令行 | 菜单栏 app + CLI 双形态 |
| 延迟配置 | 无 | 0–5 秒可调滑块 |
| 代码规模 | ~2000 行 | 核心 ~300 行 + app ~350 行 |

两者可以共存：displayplacer 适合手动切换多套布局，DisplayArrange 适合锁屏后自动防复位。

## 常见问题

**Q: 为什么我的显示器 vendor/model/serial 都是 0？**
A: 在某些沙盒或远程会话里 IOKit service 访问受限，拿不到这些信息。不影响功能，因为主键用的是 display unit，不依赖这些字段。

**Q: 换了显示器怎么办？**
A: 在系统设置里摆好新布局，点一次"保存当前布局"覆盖旧配置即可。

**Q: 三台或更多显示器支持吗？**
A: 支持，最多 32 台（`DC_MAX_DISPLAYS` 常量）。

**Q: 卸载？**
A: 退出 app 后把 `DisplayArrange.app` 拖进废纸篓，再删掉配置目录：
```sh
rm -rf ~/.config/displayarrange
rm -f ~/Library/LaunchAgents/com.displayarrange.app.plist
```
如果你还装过 CLI 的 watch 模式（见下节），一并卸载：
```sh
launchctl unload ~/Library/LaunchAgents/com.displayarrange.cli.plist 2>/dev/null
rm -f ~/Library/LaunchAgents/com.displayarrange.cli.plist
```

## 用 launchd 常驻 CLI watch 模式（可选）

如果你不想装 app、只想让 CLI 在后台监听解锁事件，可以用 `packaging/` 里的 LaunchAgent 模板：

```sh
make cli
sudo make install          # 默认装到 /usr/local/bin/displayarrange

# 按需编辑模板里的路径、参数和延迟，然后安装
cp packaging/com.displayarrange.cli.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.displayarrange.cli.plist
```

模板默认监听 `~/.config/displayarrange/layout.json`，日志写在 `/tmp/displayarrange.log`。菜单栏 app 自带解锁监听，两者选一个用即可，不要同时跑。

## 文件结构

```
core/
  DisplayCore.h / .c            显示器枚举、匹配、应用的核心引擎（纯 C）
cli/
  cli.c                         命令行界面
app/
  DisplayArrangeApp.swift       菜单栏 app（AppKit，单文件）
  Info.plist                    app bundle 配置
packaging/
  com.displayarrange.cli.plist  CLI watch 模式的 LaunchAgent 模板
Makefile                        一条命令同时编译 CLI 和 app
```

## License

MIT，见 [LICENSE](LICENSE)。

---

## English

> A macOS menu-bar utility that remembers your multi-monitor layout and restores it automatically after every screen unlock.

### The problem

macOS often resets multi-monitor arrangements to the default side-by-side layout after sleep or lock, because it keys layouts by `CGDirectDisplayID`, which is reassigned on every wake.

### The fix

DisplayArrange uses the **display unit** (a stable per-port identifier) instead of the ephemeral display ID, so it always knows which physical monitor goes where.

### Install

**Requires macOS 13+. Universal binary — runs natively on both Apple Silicon and Intel Macs.**

Download `DisplayArrange.app.zip` from [Releases](https://github.com/flashsoft/DisplayArrange/releases), unzip, and drag to your Applications folder. CLI-only: grab `DisplayArrange-macos-universal.zip`. Or build from source:

```sh
git clone https://github.com/flashsoft/DisplayArrange.git
cd DisplayArrange
make app
open dist/DisplayArrange.app
```

### Usage

Click the dual-monitor icon in the menu bar:

- **Save Current Layout** (`⌘S`) — snapshot the current arrangement
- **Restore Now** (`⌘R`) — put it back immediately
- **Auto-restore on unlock** — toggle automatic recovery
- **Delay slider** — wait 0–5 s after unlock before restoring
- **Launch at login** — start automatically

### CLI

A feature-identical command-line tool is also included:

```sh
make cli
./dist/displayarrange list
./dist/displayarrange save
./dist/displayarrange apply
./dist/displayarrange watch --debounce 1000
```

The CLI and the app share the same config file at `~/.config/displayarrange/layout.json`.

### License

MIT
