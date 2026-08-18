//
// DisplayArrangeApp.swift
//
// Menu-bar app that restores your multi-monitor layout after every
// screen unlock. Single-file Swift, built with swiftc into a .app
// bundle (no .xcodeproj required).
//

import AppKit
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
    @Published var lastStatus: String = "就绪"

    private init() {
        let d = UserDefaults.standard
        self.autoRestore   = d.object(forKey: "autoRestore") as? Bool ?? true
        self.delaySeconds  = d.object(forKey: "delaySeconds") as? Double ?? 0.8
        self.launchAtLogin = d.object(forKey: "launchAtLogin") as? Bool ?? false
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
        var saved = DCDisplayList()
        var current = DCDisplayList()
        guard dc_load_file(&saved, nil) != 0,
              dc_collect(&current) != 0 else { return false }
        guard dc_layouts_equal(&saved, &current) == 0 else { return true }
        return dc_apply(&saved, verbose ? 1 : 0) == 0
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
    private var delaySlider: NSSlider!
    private var delayLabel: NSTextField!
    private var loginItem: NSMenuItem!
    private var statusTextItem: NSMenuItem!

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)

        statusItem = NSStatusBar.system.statusItem(
            withLength: NSStatusItem.variableLength)
        updateIcon()

        buildMenu()
        statusItem.menu = menu

        // First launch: capture a baseline layout so the user has
        // something to restore even before pressing "save".
        if !CoreAction.hasSavedLayout() {
            if CoreAction.saveLayout() {
                Settings.shared.lastStatus = "已自动保存当前布局"
            }
        }

        // Listen for unlock events.
        DistributedNotificationCenter.default().addObserver(
            self,
            selector: #selector(onUnlock),
            name: NSNotification.Name("com.apple.screenIsUnlocked"),
            object: nil)

        refreshUI()
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

        // Save / restore
        let save = NSMenuItem(title: "保存当前布局",
                              action: #selector(onSave),
                              keyEquivalent: "s")
        save.target = self
        menu.addItem(save)

        let restore = NSMenuItem(title: "立即恢复布局",
                                 action: #selector(onRestore),
                                 keyEquivalent: "r")
        restore.target = self
        menu.addItem(restore)
        menu.addItem(.separator())

        // Auto-restore toggle
        toggleItem = NSMenuItem(title: "解锁后自动恢复",
                                action: #selector(onToggleAuto),
                                keyEquivalent: "")
        toggleItem.target = self
        menu.addItem(toggleItem)

        // Delay slider
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
        quit.target = self
        menu.addItem(quit)
    }

    private func refreshUI() {
        let s = Settings.shared
        toggleItem.state = s.autoRestore ? .on : .off
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

    @objc private func onToggleAuto() {
        Settings.shared.autoRestore.toggle()
        refreshUI()
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
