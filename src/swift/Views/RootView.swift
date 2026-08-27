// RootView.swift — Root view switching between menu and game
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct RootView: View {
    @State private var appState = AppState.shared
    @State private var fileImporter = FileImportHandler.shared

    var body: some View {
        ZStack {
            switch appState.currentScreen {
            case .menu:
                Color(uiColor: .systemGroupedBackground)
                    .ignoresSafeArea()
                MenuTabView()
            case .playing:
                GameScreenView()
            }
        }
        .onOpenURL { url in
            fileImporter.handleURL(url)
        }
        .alert("File Import", isPresented: $fileImporter.showImportAlert) {
            Button("OK") {}
        } message: {
            Text(fileImporter.lastImportMessage ?? "")
        }
    }
}

struct MenuTabView: View {
    @State private var appState = AppState.shared
    @State private var selectedTab = 0

    var body: some View {
        TabView(selection: $selectedTab) {
            GameListView()
                .tabItem {
                    Label("Games", systemImage: "gamecontroller")
                }
                .tag(0)

            BIOSListView()
                .tabItem {
                    Label("BIOS", systemImage: "cpu")
                }
                .tag(1)

            // [TWEAK] Replaces the old Help tab. Its guidance did not need a permanent tab —
            // it now sits where each question comes up (the library's empty state, the folder
            // list, the settings footers) plus a Help row under Settings. Per-game settings and
            // cheats did need one: they are per game, so they cannot live in global Settings.
            TweaksListView()
                .tabItem {
                    Label("Tweaks", systemImage: "wand.and.stars")
                }
                .tag(2)

            NavigationStack {
                SettingsRootView()
            }
            .tabItem {
                Label("Settings", systemImage: "gearshape")
            }
            .tag(3)
        }
        .tint(.blue)
    }
}
