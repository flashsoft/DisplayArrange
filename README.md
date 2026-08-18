# displayarrange

> macOS 多显示器布局救星：锁屏/睡眠后自动恢复双屏排列，不再每次手动拖。

[English](#english) | 中文

---

## 它解决什么问题

macOS 在锁屏、睡眠或显示器重新握手后，经常把多显示器的相对位置重置成默认的"左右并排"。这是因为系统按 `CGDirectDisplayID` 记忆布局，而这个 ID 每次唤醒都可能重新分配，导致"认不出"原来的屏幕组合。

`displayarrange` 是一个约 500 行的单文件 C 工具，用 **display unit**（绑定物理端口的稳定标识）而不是会漂移的 display ID 来识别显示器，所以唤醒后 ID 重排也能准确还原你的布局。

## 特性

- **稳定识别**：主键用 `CGDisplayUnitNumber`，重启/唤醒/ID 重排都不会认错屏幕
- **自动恢复**：watch 模式监听 `com.apple.screenIsUnlocked`，解锁后自动摆正
- **手动触发**：`apply` / `apply-if-different` 随时手动恢复
- **零依赖**：单文件 C，只链 CoreGraphics / CoreFoundation / IOKit 系统框架
- **JSON 配置**：人类可读的布局文件，存 `~/.config/displayarrange/layout.json`
- **LaunchAgent 常驻**：开机自启，后台静默运行

## 快速开始

### 编译

```sh
cc -O2 -o displayarrange displayarrange.c \
   -framework CoreGraphics -framework CoreFoundation -framework IOKit
```

### 使用

```sh
# 1. 在 系统设置 > 显示器 里摆好你的双屏位置，然后：
./displayarrange save

# 2. 锁屏再解锁，看看布局有没有被重置：
./displayarrange list          # 查看当前布局
./displayarrange apply         # 手动恢复

# 3. 让它自动恢复：
./displayarrange watch &
```

### 开机自启（推荐）

```sh
# 把二进制放到稳定位置
mkdir -p ~/bin
cp displayarrange ~/bin/

# 装 LaunchAgent
cp com.user.displayarrange.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.user.displayarrange.plist
```

日志写到 `/tmp/displayarrange.log`。

停止：

```sh
launchctl unload ~/Library/LaunchAgents/com.user.displayarrange.plist
```

## 命令一览

| 命令 | 作用 |
|------|------|
| `list` | 列出所有连接的显示器（unit、原点、分辨率） |
| `save [path]` | 保存当前布局到 JSON（默认 `~/.config/displayarrange/layout.json`） |
| `apply [path]` | 立即恢复保存的布局 |
| `apply-if-different [path]` | 仅在布局漂移时恢复（脚本里用） |
| `watch [path]` | 后台监听解锁事件，自动恢复 |

### watch 参数

- `--debounce N`：解锁后等 N 毫秒再操作（默认 800，给显示器重新握手留时间）
- `--verbose`：打印详细日志

## 工作原理

1. **枚举**：用 `CGGetOnlineDisplayList` 而不是 active list，睡着的显示器也能看到
2. **身份**：主键是 `CGDisplayUnitNumber`（物理端口稳定 ID），vendor/model/serial 做兜底
3. **布局**：JSON 里存每台显示器的 unit、原点坐标、分辨率
4. **应用**：走 `CGBeginDisplayConfiguration` + `CGConfigureDisplayOrigin` + `CGCompleteDisplayConfiguration`，和系统自己用的是同一套 API
5. **监听**：订阅 `com.apple.screenIsUnlocked` 分布式通知，触发后先比对再决定要不要恢复

## 和 jakehilborn/displayplacer 的区别

| 维度 | displayplacer | displayarrange |
|------|---------------|----------------|
| 显示器识别 | `CGDirectDisplayID`（会漂移） | `CGDisplayUnitNumber`（端口级稳定） |
| 自动恢复 | 无，需配合第三方工具 | 内置 `watch` 模式，原生监听解锁事件 |
| 代码规模 | ~2000 行 | ~500 行单文件 |
| 依赖 | 无 | 无 |
| 适用场景 | 手动/脚本触发布局切换 | 锁屏后自动防复位 |

两者可以共存：displayplacer 适合做布局切换，displayarrange 适合防锁屏复位。

## 常见问题

**Q: 为什么我的显示器 vendor/model/serial 都是 0？**
A: 在某些沙盒或远程会话里 IOKit service 访问受限，拿不到这些信息。不影响功能，因为主键用的是 display unit，不依赖这些字段。

**Q: 换了显示器怎么办？**
A: 在系统设置里摆好新布局，执行 `displayarrange save` 覆盖旧配置即可。

**Q: 三台或更多显示器支持吗？**
A: 支持，最多 32 台（`MAX_DISPLAYS` 常量）。

**Q: 卸载？**
A:
```sh
launchctl unload ~/Library/LaunchAgents/com.user.displayarrange.plist
rm ~/Library/LaunchAgents/com.user.displayarrange.plist
rm ~/bin/displayarrange
rm -rf ~/.config/displayarrange
```

## License

MIT，见 [LICENSE](LICENSE)。

---

## English

> A tiny macOS utility that remembers your multi-monitor layout and restores it automatically after every screen unlock.

### The problem

macOS often resets multi-monitor arrangements to the default side-by-side layout after sleep or lock, because it keys layouts by `CGDirectDisplayID`, which is reassigned on every wake.

### The fix

`displayarrange` uses the **display unit** (a stable per-port identifier) instead of the ephemeral display ID, so it always knows which physical monitor goes where.

### Build & use

```sh
cc -O2 -o displayarrange displayarrange.c \
   -framework CoreGraphics -framework CoreFoundation -framework IOKit

# Arrange monitors in System Settings, then:
./displayarrange save

# Auto-restore after every unlock:
./displayarrange watch &
```

See the Chinese section above for the full command reference, LaunchAgent setup, and FAQ. The commands and config paths are identical.

### License

MIT
