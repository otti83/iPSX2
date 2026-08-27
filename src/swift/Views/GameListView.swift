// GameListView.swift — game library backed by upstream GameList
// SPDX-License-Identifier: GPL-3.0+
//
// [F8] This used to list raw ISO filenames from two hardcoded directories, because
// GameList::Refresh had no caller and GameList's entries were always empty. It now reads real
// entries: GameDB titles, serial, CRC, region, compatibility rating, size, play time and
// cover art.

import SwiftUI
import UniformTypeIdentifiers

struct GameEntry: Identifiable, Hashable {
    let path: String
    let serial: String
    let title: String
    let titleEn: String
    let region: String
    let type: String
    let crc: UInt32
    let size: UInt64
    let compatibility: Int
    let lastPlayed: Int64
    let totalPlayed: Int64
    let coverPath: String

    // The path is the identity: titles are localised and not unique across regions.
    var id: String { path }
    var isFavorite: Bool { iPSX2Bridge.isFavorite(path) }

    init(_ d: [String: Any]) {
        path          = d["path"] as? String ?? ""
        serial        = d["serial"] as? String ?? ""
        title         = d["title"] as? String ?? ""
        titleEn       = d["titleEn"] as? String ?? ""
        region        = d["region"] as? String ?? ""
        type          = d["type"] as? String ?? ""
        crc           = (d["crc"] as? NSNumber)?.uint32Value ?? 0
        size          = (d["size"] as? NSNumber)?.uint64Value ?? 0
        compatibility = (d["compatibility"] as? NSNumber)?.intValue ?? 0
        lastPlayed    = (d["lastPlayed"] as? NSNumber)?.int64Value ?? 0
        totalPlayed   = (d["totalPlayed"] as? NSNumber)?.int64Value ?? 0
        coverPath     = d["coverPath"] as? String ?? ""
    }

    /// GameDatabaseSchema::Compatibility — 0 is Unknown, 5 is Perfect.
    var compatibilityLabel: String {
        switch compatibility {
        case 1: return "Nothing"
        case 2: return "Intro"
        case 3: return "Menu"
        case 4: return "In-Game"
        case 5: return "Playable"
        case 6: return "Perfect"
        default: return "Unknown"
        }
    }

    var compatibilityColor: Color {
        switch compatibility {
        case 1, 2: return .red
        case 3, 4: return .orange
        case 5: return .yellow
        case 6: return .green
        default: return .gray
        }
    }
}

enum GameListLayout: String {
    case list, grid
}

struct GameListView: View {
    @State private var games: [GameEntry] = []
    @State private var appState = AppState.shared
    @State private var showRestartAlert = false
    @State private var showStopAlert = false
    @State private var pendingEntry: GameEntry? = nil
    @State private var pendingBIOS = false
    @State private var isRefreshing = false
    @State private var showCoverDownload = false
    @State private var coverTarget: GameEntry? = nil
    @AppStorage("gameListLayout") private var layoutRaw = GameListLayout.list.rawValue

    private var layout: GameListLayout { GameListLayout(rawValue: layoutRaw) ?? .list }

    private var sortedGames: [GameEntry] {
        games.sorted { a, b in
            if a.isFavorite != b.isFavorite { return a.isFavorite }
            return a.title.localizedCaseInsensitiveCompare(b.title) == .orderedAscending
        }
    }

    var body: some View {
        NavigationStack {
            Group {
                if games.isEmpty && appState.runningGameName == nil {
                    emptyState
                } else if layout == .grid {
                    gridBody
                } else {
                    listBody
                }
            }
            .navigationTitle("Games")
            .toolbar { toolbarContent }
            .alert("Restart VM?", isPresented: $showRestartAlert) {
                Button("Cancel", role: .cancel) {}
                Button("Restart", role: .destructive) {
                    if pendingBIOS {
                        appState.shutdownAndBootBIOS()
                    } else if let e = pendingEntry {
                        appState.shutdownAndBoot(path: e.path, title: e.title)
                    }
                }
            } message: {
                Text("VM is currently running.\nShut down and start \(pendingBIOS ? "BIOS Only" : (pendingEntry?.title ?? ""))?")
            }
            .alert("Stop Emulation?", isPresented: $showStopAlert) {
                Button("Cancel", role: .cancel) {}
                Button("Stop", role: .destructive) {
                    iPSX2Bridge.requestVMStop()
                    appState.runningGameName = nil
                    appState.runningGamePath = nil
                }
            } message: {
                Text("This will shut down the running game. All unsaved progress will be lost.")
            }
        }
        .sheet(isPresented: $showCoverDownload) {
            CoverDownloadView()
        }
        .fileImporter(
            isPresented: Binding(get: { coverTarget != nil }, set: { if !$0 { coverTarget = nil } }),
            allowedContentTypes: [.png, .jpeg, .image]
        ) { result in
            if case .success(let url) = result, let target = coverTarget {
                // A picked file lives outside the sandbox until we ask for access.
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                _ = iPSX2Bridge.setCoverForEntryPath(target.path, fromFile: url.path)
                reload()
            }
            coverTarget = nil
        }
        .onAppear {
            reload()
            // Entries are only ever populated by a scan, so kick one off if we have none.
            if games.isEmpty { refresh(invalidateCache: false) }
        }
        .onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("iPSX2GameListDidRefresh"))) { _ in
            isRefreshing = false
            reload()
        }
        // A finished download has written files the entries were built before, and nothing else
        // re-reads them: dismissing a sheet does not re-run the presenting view's onAppear, so
        // without this the covers sit on disk while the list keeps drawing placeholders until the
        // next rescan. Observed here, not assumed. It is on this view rather than the sheet's
        // onDismiss so that a swipe-away mid-download is still picked up.
        .onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("iPSX2CoverDownloadDidFinish"))) { _ in
            reload()
        }
    }

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        ToolbarItem(placement: .topBarTrailing) {
            Button { refresh(invalidateCache: true) } label: {
                if isRefreshing {
                    ProgressView()
                } else {
                    Image(systemName: "arrow.clockwise")
                }
            }
            .disabled(isRefreshing)
        }
        ToolbarItem(placement: .topBarTrailing) {
            Button {
                layoutRaw = (layout == .list ? GameListLayout.grid : GameListLayout.list).rawValue
            } label: {
                Image(systemName: layout == .list ? "square.grid.2x2" : "list.bullet")
            }
        }
        ToolbarItem(placement: .topBarTrailing) {
            Menu {
                Button {
                    showCoverDownload = true
                } label: { Label("Download Covers…", systemImage: "photo.on.rectangle.angled") }
                Divider()
                NavigationLink {
                    GameFoldersView()
                } label: { Label("Game Folders…", systemImage: "folder") }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
        }
        ToolbarItem(placement: .topBarLeading) {
            if iPSX2Bridge.hasBIOS() {
                Button("BIOS Only") {
                    if appState.runningGameName == "BIOS" {
                        appState.returnToGame()
                    } else if appState.runningGameName != nil {
                        pendingBIOS = true; pendingEntry = nil; showRestartAlert = true
                    } else {
                        appState.bootBIOSOnly()
                    }
                }
                .font(.caption)
            }
        }
    }

    private var listBody: some View {
        List {
            if let name = appState.runningGameName {
                vmStatusSection(gameName: name)
            }
            ForEach(sortedGames) { game in
                Button { launch(game) } label: { listRow(game) }
                    .foregroundStyle(.primary)
                    .contextMenu { entryMenu(game) }
            }
        }
    }

    private var gridBody: some View {
        ScrollView {
            if let name = appState.runningGameName {
                // Stop has to be here too, not just in the list layout — a running VM must be
                // stoppable from whichever layout the user happens to be in. The card, padding
                // and divider reproduce what a List Section gives the list layout for free, so
                // the two layouts present the running VM identically instead of one having the
                // Stop button floating loose outside the card.
                VStack(spacing: 0) {
                    resumeBanner(gameName: name)
                    Divider().padding(.leading, 12)
                    stopButton.padding(.vertical, 11)
                }
                .padding(8)
                .background(
                    Color(uiColor: .secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 12)
                )
                .padding(.horizontal)
                .padding(.bottom, 8)
            }
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 130), spacing: 16)], spacing: 16) {
                ForEach(sortedGames) { game in
                    Button { launch(game) } label: { gridCell(game) }
                        .buttonStyle(.plain)
                        .contextMenu { entryMenu(game) }
                }
            }
            .padding()
        }
    }

    private func cover(_ game: GameEntry, width: CGFloat) -> some View {
        Group {
            if !game.coverPath.isEmpty, let img = UIImage(contentsOfFile: game.coverPath) {
                Image(uiImage: img).resizable().aspectRatio(contentMode: .fill)
            } else {
                ZStack {
                    Rectangle().fill(.quaternary)
                    Image(systemName: "opticaldisc")
                        .font(.system(size: width * 0.35))
                        .foregroundStyle(.secondary)
                }
            }
        }
        .frame(width: width, height: width * 1.4)
        .clipShape(RoundedRectangle(cornerRadius: 6))
    }

    private func listRow(_ game: GameEntry) -> some View {
        HStack(spacing: 12) {
            cover(game, width: 44)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(game.title).font(.body).lineLimit(1)
                    if game.path == appState.runningGamePath {
                        Image(systemName: "circle.fill").font(.system(size: 8)).foregroundStyle(.green)
                    }
                }
                HStack(spacing: 6) {
                    Text(game.serial.isEmpty ? "—" : game.serial)
                    Text("·")
                    Text(game.region)
                    Text("·")
                    Text(formatSize(game.size))
                }
                .font(.caption)
                .foregroundStyle(.secondary)
                HStack(spacing: 6) {
                    Circle().fill(game.compatibilityColor).frame(width: 6, height: 6)
                    Text(game.compatibilityLabel)
                    if game.totalPlayed > 0 {
                        Text("·")
                        Text(formatPlaytime(game.totalPlayed))
                    }
                }
                .font(.caption2)
                .foregroundStyle(.tertiary)
            }
            Spacer()
            Button { toggleFavorite(game) } label: {
                Image(systemName: game.isFavorite ? "star.fill" : "star")
                    .foregroundStyle(game.isFavorite ? .yellow : .gray)
            }
            .buttonStyle(.plain)
        }
    }

    private func gridCell(_ game: GameEntry) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            ZStack(alignment: .topTrailing) {
                cover(game, width: 130)
                if game.isFavorite {
                    Image(systemName: "star.fill")
                        .foregroundStyle(.yellow)
                        .padding(6)
                        .shadow(radius: 2)
                }
                if game.path == appState.runningGamePath {
                    VStack {
                        Spacer()
                        HStack {
                            Image(systemName: "play.circle.fill").foregroundStyle(.green)
                            Spacer()
                        }
                        .padding(6)
                    }
                    .frame(height: 130 * 1.4)
                }
            }
            Text(game.title).font(.caption).lineLimit(2).frame(height: 30, alignment: .top)
            HStack(spacing: 4) {
                Circle().fill(game.compatibilityColor).frame(width: 5, height: 5)
                Text(game.region).font(.caption2).foregroundStyle(.secondary)
            }
        }
        .frame(width: 130)
    }

    private func resumeBanner(gameName: String) -> some View {
        Button { appState.returnToGame() } label: {
            HStack {
                Image(systemName: "play.circle.fill").foregroundStyle(.green).font(.title)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Now Running").font(.caption).foregroundStyle(.secondary)
                    Text(gameName == "BIOS" ? "BIOS Only" : gameName).font(.body).fontWeight(.semibold)
                }
                Spacer()
                Text("Resume").font(.subheadline).fontWeight(.medium)
            }
            .padding()
            .background(.quaternary, in: RoundedRectangle(cornerRadius: 10))
        }
        .tint(.primary)
    }

    private var stopButton: some View {
        Button(role: .destructive) { showStopAlert = true } label: {
            HStack {
                Spacer()
                Label("Stop Emulation", systemImage: "stop.circle").font(.subheadline)
                Spacer()
            }
        }
    }

    private func vmStatusSection(gameName: String) -> some View {
        Section {
            resumeBanner(gameName: gameName)
            stopButton
        }
    }

    // The old text here named CUE as a supported format — nothing in the tree reads a CUE sheet,
    // so anyone who followed it waited for a game that was never going to appear. The folders and
    // the extension list below are the ones the scanner actually uses.
    private var emptyState: some View {
        ScrollView {
            VStack(spacing: 18) {
                Image(systemName: "opticaldisc").font(.system(size: 48)).foregroundStyle(.secondary)
                Text("No Games Found").font(.title2).fontWeight(.semibold)

                VStack(alignment: .leading, spacing: 12) {
                    folderHint(
                        icon: "folder.fill",
                        title: "Documents/iso/",
                        detail: "Scanned including subfolders — organise games however you like."
                    )
                    folderHint(
                        icon: "folder",
                        title: "Documents/",
                        detail: "Scanned at the top level only."
                    )
                    folderHint(
                        icon: "folder.badge.plus",
                        title: "Folders you add",
                        detail: "Settings › Game Folders can point the library at anywhere else you keep games."
                    )
                }
                .frame(maxWidth: 420)

                VStack(spacing: 6) {
                    Text("Supported formats")
                        .font(.subheadline).fontWeight(.medium)
                    Text("ISO · BIN · IMG · MDF · CSO · ZSO · GZ · CHD · ELF")
                        .font(.caption).foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                    Text("CUE sheets are not read. With a BIN/CUE pair, use the BIN.")
                        .font(.caption2).foregroundStyle(.tertiary)
                        .multilineTextAlignment(.center)
                }

                // Not "On My iPhone" — the same build runs on iPad, where the Files app calls
                // that location "On My iPad".
                Text("Copy files over with the Files app, or from a computer with Finder or iTunes File Sharing.")
                    .font(.caption).foregroundStyle(.tertiary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 420)

                Button("Scan Again") { refresh(invalidateCache: true) }
                    .buttonStyle(.bordered)
                    .disabled(isRefreshing)
            }
            .padding()
            .frame(maxWidth: .infinity)
        }
    }

    private func folderHint(icon: String, title: String, detail: String) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: icon)
                .foregroundStyle(.blue)
                .frame(width: 22)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.subheadline).fontWeight(.medium)
                Text(detail).font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private func entryMenu(_ game: GameEntry) -> some View {
        Button {
            coverTarget = game
        } label: { Label("Set Cover from File…", systemImage: "photo") }
        Button {
            showCoverDownload = true
        } label: { Label("Download Covers…", systemImage: "photo.on.rectangle.angled") }
        Divider()
        Button {
            toggleFavorite(game)
        } label: {
            Label(game.isFavorite ? "Remove Favourite" : "Add Favourite",
                  systemImage: game.isFavorite ? "star.slash" : "star")
        }
    }

    private func launch(_ game: GameEntry) {
        if game.path == appState.runningGamePath {
            appState.returnToGame()
        } else if appState.runningGameName != nil {
            pendingEntry = game; pendingBIOS = false; showRestartAlert = true
        } else {
            appState.bootGame(path: game.path, title: game.title)
        }
    }

    private func reload() {
        games = iPSX2Bridge.gameListEntries().map { GameEntry($0) }
    }

    private func refresh(invalidateCache: Bool) {
        // Scanning reassigns the global CDVD reader, so the core refuses while a VM is alive.
        guard !iPSX2Bridge.isVMRunning() else { return }
        isRefreshing = true
        iPSX2Bridge.refreshGameList(invalidateCache)
    }

    private func toggleFavorite(_ game: GameEntry) {
        iPSX2Bridge.setFavorite(game.path, favorite: !game.isFavorite)
        reload()
    }

    private func formatSize(_ bytes: UInt64) -> String {
        let gb = Double(bytes) / 1_073_741_824
        if gb >= 1.0 { return String(format: "%.1f GB", gb) }
        return String(format: "%.0f MB", Double(bytes) / 1_048_576)
    }

    private func formatPlaytime(_ seconds: Int64) -> String {
        let h = seconds / 3600, m = (seconds % 3600) / 60
        return h > 0 ? "\(h)h \(m)m" : "\(m)m"
    }
}
