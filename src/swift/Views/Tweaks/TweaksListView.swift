// TweaksListView.swift — pick a game to tweak
// SPDX-License-Identifier: GPL-3.0+
//
// The tab root. Everything here is per game, so the first thing it has to do is ask which game.

import SwiftUI

struct TweaksListView: View {
    @State private var games: [GameEntry] = []
    @State private var appState = AppState.shared
    @State private var search = ""

    private var sortedGames: [GameEntry] {
        let base = games.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
        guard !search.isEmpty else { return base }
        return base.filter {
            $0.title.localizedCaseInsensitiveContains(search) ||
            $0.serial.localizedCaseInsensitiveContains(search)
        }
    }

    var body: some View {
        NavigationStack {
            Group {
                if games.isEmpty {
                    emptyState
                } else {
                    list
                }
            }
            .navigationTitle("Tweaks")
        }
        .onAppear(perform: reload)
        .onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("iPSX2GameListDidRefresh"))) { _ in
            reload()
        }
    }

    private var list: some View {
        List {
            if let running = sortedGames.first(where: { $0.path == appState.runningGamePath }) {
                Section {
                    NavigationLink { GameTweaksView(game: running) } label: { row(running, running: true) }
                } header: {
                    Text("Now Running")
                } footer: {
                    Text("Changes here apply to the running game as soon as you leave this screen.")
                }
            }

            Section {
                ForEach(sortedGames) { game in
                    NavigationLink { GameTweaksView(game: game) } label: { row(game, running: false) }
                }
            } header: {
                Text("Library")
            } footer: {
                Text("Each game keeps its own settings and its own list of enabled cheat codes. Anything left alone follows the global setting from the Settings tab.")
            }
        }
        .searchable(text: $search, prompt: "Search games")
    }

    private func row(_ game: GameEntry, running: Bool) -> some View {
        HStack(spacing: 12) {
            cover(game)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(game.title).lineLimit(1)
                    if running {
                        Image(systemName: "circle.fill").font(.system(size: 8)).foregroundStyle(.green)
                    }
                }
                HStack(spacing: 6) {
                    Text(game.serial.isEmpty ? "—" : game.serial)
                    if iPSX2Bridge.gameSettingsExist(forEntryPath: game.path) {
                        Text("·")
                        Label("Customised", systemImage: "wand.and.stars")
                            .labelStyle(.titleAndIcon)
                            .foregroundStyle(.blue)
                    }
                }
                .font(.caption)
                .foregroundStyle(.secondary)
            }
        }
    }

    private func cover(_ game: GameEntry) -> some View {
        Group {
            if !game.coverPath.isEmpty, let img = UIImage(contentsOfFile: game.coverPath) {
                Image(uiImage: img).resizable().aspectRatio(contentMode: .fill)
            } else {
                ZStack {
                    Rectangle().fill(.quaternary)
                    Image(systemName: "opticaldisc").font(.system(size: 16)).foregroundStyle(.secondary)
                }
            }
        }
        .frame(width: 34, height: 48)
        .clipShape(RoundedRectangle(cornerRadius: 4))
    }

    private var emptyState: some View {
        VStack(spacing: 14) {
            Image(systemName: "wand.and.stars").font(.system(size: 44)).foregroundStyle(.secondary)
            Text("No Games Yet").font(.title3).fontWeight(.semibold)
            Text("Per-game settings and cheat codes are attached to a game, so add some games first.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Text("The Games tab explains where to put them.")
                .font(.caption)
                .foregroundStyle(.tertiary)
        }
        .padding(32)
    }

    private func reload() {
        games = iPSX2Bridge.gameListEntries().map { GameEntry($0) }
    }
}
