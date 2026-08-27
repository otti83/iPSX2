// AppState.swift — App screen state management
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

@Observable
final class AppState: @unchecked Sendable {
    static let shared = AppState()

    enum Screen {
        case menu
        case playing
    }

    var currentScreen: Screen = .menu
    var selectedTab: Int = 0
    var runningGameName: String? = nil
    /// [F8] Absolute path of the running entry, when it came from the game list. Titles are not
    /// unique (and are localised), so the path is what identifies "this row is running".
    var runningGamePath: String? = nil
    var hideStatusBar: Bool = false

    @ObservationIgnored private var pendingBootAction: (() -> Void)?
    @ObservationIgnored private var shutdownObserver: NSObjectProtocol?

    @ObservationIgnored private var autoBootObserver: NSObjectProtocol?

    private init() {
        shutdownObserver = NotificationCenter.default.addObserver(
            forName: NSNotification.Name("iPSX2VMDidShutdown"),
            object: nil, queue: .main
        ) { [weak self] _ in
            self?.runningGameName = nil
            self?.runningGamePath = nil
            if let action = self?.pendingBootAction {
                self?.pendingBootAction = nil
                action()
            } else {
                // No pending reboot — return to menu (VM crash / normal shutdown)
                self?.currentScreen = .menu
            }
        }

        // [P48] Auto-boot: ObjC side posts this notification to switch UI to game screen
        autoBootObserver = NotificationCenter.default.addObserver(
            forName: NSNotification.Name("iPSX2AutoBootDidStart"),
            object: nil, queue: .main
        ) { [weak self] _ in
            self?.runningGameName = "AutoBoot"
            self?.currentScreen = .playing
        }
    }

    func bootGame(isoName: String) {
        iPSX2Bridge.bootISO(isoName)
        iPSX2Bridge.requestVMBoot()
        runningGameName = isoName
        runningGamePath = nil
        currentScreen = .playing
    }

    /// [F8] Boot a game-list entry. Entries carry an absolute path and a GameDB title, so
    /// identity (the path) and what is shown to the user (the title) are no longer the same
    /// string the way they were when the list was raw filenames.
    func bootGame(path: String, title: String) {
        iPSX2Bridge.bootGame(atPath: path)
        iPSX2Bridge.requestVMBoot()
        runningGameName = title
        runningGamePath = path
        currentScreen = .playing
    }

    func bootBIOSOnly() {
        iPSX2Bridge.setINIString("GameISO", key: "BootISO", value: "")
        iPSX2Bridge.requestVMBoot()
        runningGameName = "BIOS"
        runningGamePath = nil
        currentScreen = .playing
    }

    func returnToMenu() {
        currentScreen = .menu
        // [P44-2] Restore opaque background on hosting controller
        NotificationCenter.default.post(name: NSNotification.Name("iPSX2ReturnToMenu"), object: nil)
    }

    func returnToGame() {
        if runningGameName != nil {
            // [P44-2] Clear background so Metal surface shows through
            NotificationCenter.default.post(name: NSNotification.Name("iPSX2EnterGameScreen"), object: nil)
            currentScreen = .playing
        }
    }

    func shutdownAndBoot(isoName: String) {
        pendingBootAction = { [weak self] in
            self?.bootGame(isoName: isoName)
        }
        iPSX2Bridge.requestVMShutdown()
    }

    func shutdownAndBoot(path: String, title: String) {
        pendingBootAction = { [weak self] in
            self?.bootGame(path: path, title: title)
        }
        iPSX2Bridge.requestVMShutdown()
    }

    func shutdownAndBootBIOS() {
        pendingBootAction = { [weak self] in
            self?.bootBIOSOnly()
        }
        iPSX2Bridge.requestVMShutdown()
    }
}
