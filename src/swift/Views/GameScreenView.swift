// GameScreenView.swift — Unified game screen (Metal + Virtual Pad + Menu)
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct GameScreenView: View {
    @State private var appState = AppState.shared
    @State private var settings = SettingsStore.shared
    @State private var fullScreen = false
    @State private var showSaveStates = false
    @State private var limiter: iPSX2LimiterMode = .normal
    @State private var paused = false
    /// What the OSD toggle comes back to. Seeded from the stored preset on appear so that a
    /// session which starts with the overlay off still has something to restore.
    @State private var lastOsdPreset: OsdPreset = .simple

    var body: some View {
        GeometryReader { geo in
            let isLandscape = geo.size.width > geo.size.height

            if isLandscape {
                // Landscape: always use full screen area for Metal + pad
                // so that pad coordinates match the layout editor exactly.
                ZStack {
                    MetalGameView()
                    if settings.padVisible {
                        VirtualControllerView(isLandscape: true)
                    }
                    menuOverlay(isLandscape: true)
                }
                .ignoresSafeArea()  // Always fullscreen layout in landscape
            } else {
                // Portrait: Metal top half, pad bottom half, within safe area
                VStack(spacing: 0) {
                    MetalGameView()
                        .frame(height: geo.size.height / 2)
                    if settings.padVisible {
                        VirtualControllerView()
                            .frame(height: geo.size.height / 2)
                    } else {
                        Spacer()
                    }
                }
                .overlay(alignment: .topTrailing) {
                    menuOverlay(isLandscape: false)
                }
            }
        }
        .onChange(of: fullScreen) { _, newValue in
            iPSX2Bridge.setFullScreen(newValue)
        }
        .sheet(isPresented: $showSaveStates) {
            SaveStateView()
        }
        .onAppear {
            limiter = iPSX2Bridge.limiterMode()
            paused = iPSX2Bridge.isPaused()
            if settings.osdPreset != .off { lastOsdPreset = settings.osdPreset }
        }
        .onChange(of: settings.osdPreset) { _, newValue in
            if newValue != .off { lastOsdPreset = newValue }
        }
    }

    private func menuOverlay(isLandscape: Bool) -> some View {
        VStack {
            HStack {
                Spacer()
                menuButton(isLandscape: isLandscape)
            }
            .padding(.top, isLandscape ? 8 : 4)
            .padding(.trailing, isLandscape ? 8 : 4)
            Spacer()
        }
    }

    private func setLimiter(_ mode: iPSX2LimiterMode) {
        iPSX2Bridge.setLimiterMode(mode)
        limiter = mode
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
    }

    private func menuButton(isLandscape: Bool) -> some View {
        Menu {
            // Turning the overlay back on restores whatever preset was last chosen in Settings,
            // rather than dropping to Simple and quietly discarding the user's selection.
            Toggle(isOn: Binding(
                get: { settings.osdPreset != .off },
                set: { on in settings.osdPreset = on ? lastOsdPreset : .off }
            )) {
                Label("OSD", systemImage: "speedometer")
            }
            Toggle(isOn: $settings.padVisible) {
                Label("Virtual Pad", systemImage: "gamecontroller")
            }
            if isLandscape {
                Toggle(isOn: $fullScreen) {
                    Label("Full Screen", systemImage: "arrow.up.left.and.arrow.down.right")
                }
            }
            Divider()
            // [SPEED] Fast-forward and slow-motion are toggles rather than a picker: during play
            // you want one tap to go fast and one tap to come back, not a menu of four modes.
            // Selecting one clears the other, since the limiter holds a single mode.
            Toggle(isOn: Binding(
                get: { limiter == .turbo },
                set: { on in setLimiter(on ? .turbo : .normal) }
            )) {
                Label("Fast Forward", systemImage: "forward.fill")
            }
            Toggle(isOn: Binding(
                get: { limiter == .slowMotion },
                set: { on in setLimiter(on ? .slowMotion : .normal) }
            )) {
                Label("Slow Motion", systemImage: "backward.fill")
            }
            Button {
                let next = !paused
                iPSX2Bridge.setPaused(next)
                paused = next
                UIImpactFeedbackGenerator(style: .light).impactOccurred()
            } label: {
                Label(paused ? "Resume" : "Pause", systemImage: paused ? "play.fill" : "pause.fill")
            }

            Divider()
            // [S2] Quick slot 1 is here so the common case is one tap without opening a sheet;
            // the sheet is for choosing a slot and seeing thumbnails.
            Button {
                iPSX2Bridge.saveState(toSlot: 1)
                UIImpactFeedbackGenerator(style: .medium).impactOccurred()
            } label: {
                Label("Quick Save", systemImage: "tray.and.arrow.up")
            }
            Button {
                iPSX2Bridge.loadState(fromSlot: 1, backup: false)
                UIImpactFeedbackGenerator(style: .medium).impactOccurred()
            } label: {
                Label("Quick Load", systemImage: "tray.and.arrow.down")
            }
            Button {
                showSaveStates = true
            } label: {
                Label("Save States…", systemImage: "square.stack")
            }
            Divider()
            // Several settings only take effect on a fresh boot. Without this the only way to
            // get one was to leave the game, stop the VM and start it again from the library.
            if let path = appState.runningGamePath, let title = appState.runningGameName {
                Button {
                    appState.shutdownAndBoot(path: path, title: title)
                } label: {
                    Label("Restart Game", systemImage: "arrow.clockwise")
                }
            }
            Button {
                appState.returnToMenu()
            } label: {
                Label("Back to Menu", systemImage: "list.bullet")
            }
        } label: {
            Image(systemName: "ellipsis.circle.fill")
                .font(.title3)
                .foregroundStyle(.white.opacity(0.5))
                .padding(6)
                .background(.black.opacity(0.15), in: Circle())
        }
    }
}
