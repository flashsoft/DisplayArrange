//
// DisplayArrangeApp.swift
//
// Menu-bar app that restores your multi-monitor layout after every
// screen unlock. Single-file Swift, built with swiftc into a .app
// bundle (no .xcodeproj required).
//

import AppKit
import Carbon.HIToolbox
import ServiceManagement
import UserNotifications

// MARK: - Bridge to the C engine

@_silgen_name("dc_collect")
func dc_collect(_ list: UnsafeMutablePointer<DCDisplayList>) -> Int32

@_silgen_name("dc_save_file")
func dc_save_file(_ list: UnsafePointer<DCDisplayList>,
                  _ path: UnsafePointer<CChar>?) -> Int32

@_silgen_name("dc_load_file")
func dc_load_file(_ list: UnsafeMutablePointer<DCDisplayList>,
                  _ path: UnsafePointer<CChar>?) -> Int32

@_silgen_name("dc_layouts_equal")
func dc_layouts_equal(_ a: UnsafePointer<DCDisplayList>,
                      _ b: UnsafePointer<DCDisplayList>) -> Int32

@_silgen_name("dc_apply")
func dc_apply(_ saved: UnsafePointer<DCDisplayList>,
              _ verbose: Int32) -> Int32

@_silgen_name("dc_restore")
func dc_restore(_ path: UnsafePointer<CChar>?,
                _ onlyIfDifferent: Int32,
                _ verbose: Int32,
                _ preApplyMsg: UnsafePointer<CChar>?) -> Int32

@_silgen_name("dc_set_prevent_sleep")
func dc_set_prevent_sleep(_ enable: Int32) -> Int32

@_silgen_name("dc_sleep_disabled")
func dc_sleep_disabled() -> Int32

@_silgen_name("dc_set_sleep_disabled")
func dc_set_sleep_disabled(_ disable: Int32) -> Int32

@_silgen_name("dc_sleep_sudo_ready")
func dc_sleep_sudo_ready() -> Int32

@_silgen_name("dc_sleep_setup_sudoers")
func dc_sleep_setup_sudoers() -> Int32

// MARK: - Settings

final class Settings: ObservableObject {
    static let shared = Settings()

    @Published var autoRestore: Bool {
        didSet { UserDefaults.standard.set(autoRestore, forKey: "autoRestore") }
    }
    @Published var delaySeconds: Double {
        didSet { UserDefaults.standard.set(delaySeconds, forKey: "delaySeconds") }
    }
    @Published var launchAtLogin: Bool {
        didSet {
            UserDefaults.standard.set(launchAtLogin, forKey: "launchAtLogin")
            LaunchAgentHelper.setEnabled(launchAtLogin)
        }
    }
    @Published var preventSleep: Bool {
        didSet {
            UserDefaults.standard.set(preventSleep, forKey: "preventSleep")
            _ = dc_set_prevent_sleep(preventSleep ? 1 : 0)
        }
    }
    // Only mutated on the main thread (launch alignment, toggle action).
    @Published var blockSystemSleep: Bool = false
    @Published var lastStatus: String = "就绪"

    private init() {
        let d = UserDefaults.standard
        self.autoRestore   = d.object(forKey: "autoRestore") as? Bool ?? true
        self.delaySeconds  = d.object(forKey: "delaySeconds") as? Double ?? 0.8
        self.launchAtLogin = d.object(forKey: "launchAtLogin") as? Bool ?? false
        self.preventSleep  = d.object(forKey: "preventSleep") as? Bool ?? false
    }
}

// MARK: - LaunchAgent helper

enum LaunchAgentHelper {
    private static var plistURL: URL {
        let lib = FileManager.default.urls(for: .libraryDirectory,
                                           in: .userDomainMask).first!
        return lib.appendingPathComponent(
            "LaunchAgents/com.displayarrange.app.plist")
    }

    static func setEnabled(_ enabled: Bool) {
        let fm = FileManager.default
        if enabled {
            guard let exe = Bundle.main.executablePath else { return }
            let plist: [String: Any] = [
                "Label": "com.displayarrange.app",
                "ProgramArguments": [exe],
                "RunAtLoad": true,
                "KeepAlive": false,
            ]
            try? fm.createDirectory(at: plistURL.deletingLastPathComponent(),
                                    withIntermediateDirectories: true)
            (plist as NSDictionary).write(to: plistURL, atomically: true)
            let task = Process()
            task.launchPath = "/bin/launchctl"
            task.arguments = ["load", plistURL.path]
            try? task.run()
        } else {
            let task = Process()
            task.launchPath = "/bin/launchctl"
            task.arguments = ["unload", plistURL.path]
            try? task.run()
            try? fm.removeItem(at: plistURL)
        }
    }
}

// MARK: - Core actions

enum CoreAction {
    static func saveLayout() -> Bool {
        var list = DCDisplayList()
        guard dc_collect(&list) != 0 else { return false }
        return dc_save_file(&list, nil) != 0
    }

    static func restoreLayout(verbose: Bool = false) -> Bool {
        let r = dc_restore(nil, 1, verbose ? 1 : 0, nil)
        return r == DC_RESTORE_OK || r == DC_RESTORE_ALREADY
    }

    static func displayCount() -> Int {
        var list = DCDisplayList()
        guard dc_collect(&list) != 0 else { return 0 }
        return list.count
    }

    static func hasSavedLayout() -> Bool {
        var list = DCDisplayList()
        return dc_load_file(&list, nil) != 0
    }
}

// MARK: - App delegate

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem!
    private var menu: NSMenu!

    private var toggleItem: NSMenuItem!
    private var sleepItem: NSMenuItem!
    private var delaySlider: NSSlider!
    private var delayLabel: NSTextField!
    private var loginItem: NSMenuItem!
    private var statusTextItem: NSMenuItem!

    // Blank-screen ("screensaver") state: one black borderless window
    // per screen, dismissed by any key or mouse button.
    private var blankWindows: [NSWindow] = []
    private var blankMonitors: [Any] = []

    // Global hotkey registrations (Carbon RegisterEventHotKey).
    private var hotKeyRefs: [EventHotKeyRef?] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)

        statusItem = NSStatusBar.system.statusItem(
            withLength: NSStatusItem.variableLength)
        updateIcon()

        buildMenu()
        statusItem.menu = menu
        registerHotKeys()

        // First launch: capture a baseline layout so the user has
        // something to restore even before pressing "save".
        if !CoreAction.hasSavedLayout() {
            if CoreAction.saveLayout() {
                Settings.shared.lastStatus = "已自动保存当前布局"
            }
        }

        // Re-arm the sleep-prevention assertion if the toggle was on
        // (assertions do not survive process restarts).
        if Settings.shared.preventSleep {
            _ = dc_set_prevent_sleep(1)
        }

        // The sleep toggle drives the system-wide pmset disablesleep
        // switch, which survives process restarts: align the toggle
        // with the real state instead of re-applying anything.
        Settings.shared.blockSystemSleep = (dc_sleep_disabled() == 1)

        // Listen for unlock events.
        DistributedNotificationCenter.default().addObserver(
            self,
            selector: #selector(onUnlock),
            name: NSNotification.Name("com.apple.screenIsUnlocked"),
            object: nil)

        refreshUI()
    }

    // Register ⌃⌘S / ⌃⌘R / ⌃⌘B as true global hotkeys. Menu
    // keyEquivalents only fire while the menu is open, so Carbon's
    // RegisterEventHotKey (no accessibility permission required) is
    // used to catch them system-wide. All three actions are idempotent,
    // so a press while the menu is open firing both paths is harmless.
    private func registerHotKeys() {
        var spec = EventTypeSpec(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(
            GetApplicationEventTarget(), { _, event, userData in
                guard let event, let userData else { return noErr }
                var hotKeyID = EventHotKeyID()
                GetEventParameter(event, UInt32(kEventParamDirectObject),
                                  UInt32(typeEventHotKeyID), nil,
                                  MemoryLayout<EventHotKeyID>.size, nil,
                                  &hotKeyID)
                let delegate = Unmanaged<AppDelegate>.fromOpaque(userData)
                    .takeUnretainedValue()
                delegate.handleHotKey(id: hotKeyID.id)
                return noErr
            }, 1, &spec, Unmanaged.passUnretained(self).toOpaque(), nil)

        let combos: [(key: UInt32, id: UInt32)] = [
            (UInt32(kVK_ANSI_S), 1),
            (UInt32(kVK_ANSI_R), 2),
            (UInt32(kVK_ANSI_B), 3),
        ]
        for combo in combos {
            var ref: EventHotKeyRef?
            let hotKeyID = EventHotKeyID(signature: OSType(0x4441_5252),
                                         id: combo.id)  // 'DARR'
            RegisterEventHotKey(combo.key, UInt32(cmdKey | controlKey),
                                hotKeyID, GetApplicationEventTarget(), 0,
                                &ref)
            hotKeyRefs.append(ref)
        }
    }

    func handleHotKey(id: UInt32) {
        switch id {
        case 1: onSave()
        case 2: onRestore()
        case 3: onBlankScreen()
        default: break
        }
    }

    private func updateIcon() {
        if let button = statusItem.button {
            let img = NSImage(
                systemSymbolName: "display.2",
                accessibilityDescription: "DisplayArrange")
            img?.isTemplate = true
            button.image = img
        }
    }

    private func buildMenu() {
        menu = NSMenu()

        // Status line
        statusTextItem = NSMenuItem(title: "", action: nil, keyEquivalent: "")
        statusTextItem.isEnabled = false
        menu.addItem(statusTextItem)
        menu.addItem(.separator())

        // Save / restore (shortcuts are all Control-Command to avoid
        // colliding with the front app's plain Command shortcuts)
        let save = NSMenuItem(title: "保存当前布局",
                              action: #selector(onSave),
                              keyEquivalent: "s")
        save.keyEquivalentModifierMask = [.command, .control]
        save.target = self
        menu.addItem(save)

        let restore = NSMenuItem(title: "立即恢复布局",
                                 action: #selector(onRestore),
                                 keyEquivalent: "r")
        restore.keyEquivalentModifierMask = [.command, .control]
        restore.target = self
        menu.addItem(restore)

        let blank = NSMenuItem(title: "立即黑屏",
                               action: #selector(onBlankScreen),
                               keyEquivalent: "b")
        blank.keyEquivalentModifierMask = [.command, .control]
        blank.target = self
        menu.addItem(blank)
        menu.addItem(.separator())

        // Auto-restore group: toggle + delay slider belong together
        toggleItem = NSMenuItem(title: "解锁后自动恢复",
                                action: #selector(onToggleAuto),
                                keyEquivalent: "")
        toggleItem.target = self
        menu.addItem(toggleItem)

        let delayItem = NSMenuItem()
        let delayView = NSView(frame: NSRect(x: 0, y: 0, width: 240, height: 44))

        delayLabel = NSTextField(labelWithString: "")
        delayLabel.frame = NSRect(x: 16, y: 22, width: 208, height: 16)
        delayLabel.font = NSFont.systemFont(ofSize: 11)
        delayLabel.textColor = .secondaryLabelColor
        delayView.addSubview(delayLabel)

        delaySlider = NSSlider(frame: NSRect(x: 16, y: 4, width: 208, height: 18))
        delaySlider.minValue = 0.0
        delaySlider.maxValue = 5.0
        delaySlider.target = self
        delaySlider.action = #selector(onDelayChanged)
        delayView.addSubview(delaySlider)

        delayItem.view = delayView
        menu.addItem(delayItem)
        menu.addItem(.separator())

        // System sleep toggle (separate group: system-level setting)
        sleepItem = NSMenuItem(title: "禁止休眠",
                               action: #selector(onToggleSleep),
                               keyEquivalent: "")
        sleepItem.target = self
        menu.addItem(sleepItem)
        menu.addItem(.separator())

        // Launch at login
        loginItem = NSMenuItem(title: "开机自动启动",
                               action: #selector(onToggleLogin),
                               keyEquivalent: "")
        loginItem.target = self
        menu.addItem(loginItem)
        menu.addItem(.separator())

        // Quit
        let quit = NSMenuItem(title: "退出 DisplayArrange",
                              action: #selector(onQuit),
                              keyEquivalent: "q")
        quit.keyEquivalentModifierMask = [.command, .control]
        quit.target = self
        menu.addItem(quit)
    }

    private func refreshUI() {
        let s = Settings.shared
        toggleItem.state = s.autoRestore ? .on : .off
        sleepItem.state  = s.blockSystemSleep ? .on : .off
        loginItem.state  = s.launchAtLogin ? .on : .off
        delaySlider.doubleValue = s.delaySeconds
        delayLabel.stringValue = String(
            format: "延迟 %.1f 秒后恢复", s.delaySeconds)

        let count = CoreAction.displayCount()
        let saved = CoreAction.hasSavedLayout()
        statusTextItem.title = "已连接 \(count) 台显示器" +
            (saved ? " · 布局已保存" : " · 尚未保存布局")
    }

    // MARK: Actions

    @objc private func onSave() {
        if CoreAction.saveLayout() {
            Settings.shared.lastStatus = "布局已保存"
            notify(title: "DisplayArrange", body: "当前布局已保存")
        } else {
            Settings.shared.lastStatus = "保存失败"
            notify(title: "DisplayArrange", body: "保存失败，请检查显示器连接")
        }
        refreshUI()
    }

    @objc private func onRestore() {
        if CoreAction.restoreLayout(verbose: true) {
            Settings.shared.lastStatus = "布局已恢复"
        } else {
            Settings.shared.lastStatus = "恢复失败"
            notify(title: "DisplayArrange",
                   body: "恢复失败，请先保存一个布局")
        }
        refreshUI()
    }

    @objc private func onBlankScreen() {
        guard blankWindows.isEmpty else { return }
        for screen in NSScreen.screens {
            let w = NSWindow(contentRect: screen.frame,
                             styleMask: .borderless,
                             backing: .buffered,
                             defer: false)
            w.isReleasedWhenClosed = false  // ARC owns these windows
            w.level = .screenSaver
            w.backgroundColor = .black
            w.isOpaque = true
            w.collectionBehavior = [.canJoinAllSpaces, .stationary]
            w.orderFrontRegardless()
            blankWindows.append(w)
        }
        setCursorHidden(true)

        let mask: NSEvent.EventTypeMask =
            [.keyDown, .leftMouseDown, .rightMouseDown, .otherMouseDown]
        // Defer the teardown to the next runloop turn: removing a monitor
        // from inside its own handler risks use-after-free mid-dispatch.
        if let m = NSEvent.addLocalMonitorForEvents(matching: mask,
                                                    handler: { [weak self] _ in
            DispatchQueue.main.async { self?.dismissBlankScreen() }
            return nil
        }) { blankMonitors.append(m) }
        if let m = NSEvent.addGlobalMonitorForEvents(matching: mask,
                                                     handler: { [weak self] _ in
            DispatchQueue.main.async { self?.dismissBlankScreen() }
        }) { blankMonitors.append(m) }
    }

    private func dismissBlankScreen() {
        guard !blankWindows.isEmpty else { return }
        for m in blankMonitors { NSEvent.removeMonitor(m) }
        blankMonitors.removeAll()
        for w in blankWindows {
            w.orderOut(nil)
            w.close()
        }
        blankWindows.removeAll()
        setCursorHidden(false)
    }

    // Display-wide cursor hide/show: works regardless of which app is
    // active, unlike NSCursor.hide() which only applies to our windows.
    private func setCursorHidden(_ hidden: Bool) {
        var ids = [CGDirectDisplayID](repeating: 0, count: 32)
        var count: UInt32 = 0
        guard CGGetOnlineDisplayList(32, &ids, &count) == .success else {
            return
        }
        for i in 0..<Int(count) {
            if hidden {
                CGDisplayHideCursor(ids[i])
            } else {
                CGDisplayShowCursor(ids[i])
            }
        }
    }

    @objc private func onToggleAuto() {
        Settings.shared.autoRestore.toggle()
        refreshUI()
    }

    @objc private func onToggleSleep() {
        let s = Settings.shared
        let target = !s.blockSystemSleep

        // First time enabling: offer one-time passwordless setup so
        // future toggles (and quit-restore) never ask for a password.
        if target && dc_sleep_sudo_ready() == 0 {
            let alert = NSAlert()
            alert.messageText = "开启免密控制？"
            alert.informativeText =
                "首次开启需要管理员授权来配置系统，之后切换「禁止休眠」" +
                "将不再需要输入密码。"
            alert.addButton(withTitle: "配置并开启")
            alert.addButton(withTitle: "仅本次开启")
            alert.addButton(withTitle: "取消")
            switch alert.runModal() {
            case .alertFirstButtonReturn:
                s.lastStatus = "正在配置免密…"
                DispatchQueue.global(qos: .userInitiated).async {
                    let ok = dc_sleep_setup_sudoers() != 0
                    DispatchQueue.main.async {
                        if ok {
                            self.applySleepDisabled(true)
                        } else {
                            s.lastStatus = "配置失败，本次仍需密码"
                            self.applySleepDisabled(true)
                        }
                    }
                }
                return
            case .alertSecondButtonReturn:
                break   // fall through to one-shot password path
            default:
                return  // cancel
            }
        }
        applySleepDisabled(target)
    }

    private func applySleepDisabled(_ target: Bool) {
        let s = Settings.shared
        s.lastStatus = target ? "正在禁用系统睡眠…" : "正在恢复系统睡眠…"
        DispatchQueue.global(qos: .userInitiated).async {
            let ok = dc_set_sleep_disabled(target ? 1 : 0) != 0
            DispatchQueue.main.async {
                if ok {
                    s.blockSystemSleep = target
                    s.lastStatus = target ? "系统睡眠已禁用" : "系统睡眠已恢复"
                } else {
                    s.lastStatus = "操作未完成（授权已取消或失败）"
                }
                self.refreshUI()
            }
        }
    }

    @objc private func onToggleLogin() {
        Settings.shared.launchAtLogin.toggle()
        refreshUI()
    }

    @objc private func onDelayChanged() {
        // Snap to 0.1 s steps.
        let v = (delaySlider.doubleValue * 10).rounded() / 10
        Settings.shared.delaySeconds = v
        delayLabel.stringValue = String(format: "延迟 %.1f 秒后恢复", v)
    }

    @objc private func onQuit() {
        NSApp.terminate(nil)
    }

    // Restore system sleep on the way out if this app disabled it.
    // With passwordless sudo configured this is silent; without it,
    // the fallback osascript path still needs the admin dialog.
    func applicationShouldTerminate(
        _ sender: NSApplication
    ) -> NSApplication.TerminateReply {
        guard dc_sleep_disabled() == 1 else { return .terminateNow }

        DispatchQueue.global(qos: .userInitiated).async {
            _ = dc_set_sleep_disabled(0)
            DispatchQueue.main.async {
                NSApp.reply(toApplicationShouldTerminate: true)
            }
        }
        return .terminateLater
    }

    @objc private func onUnlock() {
        let s = Settings.shared
        guard s.autoRestore else { return }

        let delay = s.delaySeconds
        DispatchQueue.global(qos: .userInitiated).asyncAfter(
            deadline: .now() + delay) {
            if CoreAction.restoreLayout(verbose: false) {
                DispatchQueue.main.async {
                    s.lastStatus = String(
                        format: "已于解锁后 %.1f 秒自动恢复", delay)
                    self.refreshUI()
                }
            }
        }
    }

    private func notify(title: String, body: String) {
        let center = UNUserNotificationCenter.current()
        center.requestAuthorization(options: [.alert]) { _, _ in }
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        let req = UNNotificationRequest(
            identifier: UUID().uuidString,
            content: content,
            trigger: nil)
        center.add(req)
    }
}

// MARK: - Entry point

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()
