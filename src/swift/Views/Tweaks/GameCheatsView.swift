// GameCheatsView.swift — the cheat codes available for one game
// SPDX-License-Identifier: GPL-3.0+
//
// Codes are read out of .pnach files in the cheats folder, matched on the game's serial and CRC.
// Which ones are on is stored in the same per-game file as everything else on the parent screen,
// so this view relies on that screen having the edit session open — it is only ever pushed from
// there.

import SwiftUI
import UniformTypeIdentifiers

struct GameCheat: Identifiable, Hashable {
    let name: String
    let displayName: String
    let group: String
    let details: String
    let author: String
    let enabled: Bool
    let globallyToggleable: Bool

    var id: String { name }

    init(_ d: [String: Any]) {
        name               = d["name"] as? String ?? ""
        displayName        = d["displayName"] as? String ?? ""
        group              = d["group"] as? String ?? ""
        details            = d["description"] as? String ?? ""
        author             = d["author"] as? String ?? ""
        enabled            = (d["enabled"] as? NSNumber)?.boolValue ?? false
        globallyToggleable = (d["globallyToggleable"] as? NSNumber)?.boolValue ?? false
    }
}

struct GameCheatsView: View {
    let game: GameEntry
    /// Shared with the parent so a change here also refreshes the "Enable Cheats" row there.
    @Binding var revision: Int

    @State private var cheats: [GameCheat] = []
    @State private var unlabelled = 0
    @State private var showImporter = false
    @State private var importMessage: String?

    /// Group name -> codes, with the ungrouped ones first under an empty key.
    private var grouped: [(String, [GameCheat])] {
        let groups = Dictionary(grouping: cheats, by: \.group)
        return groups.sorted { a, b in
            if a.key.isEmpty != b.key.isEmpty { return a.key.isEmpty }
            return a.key.localizedCaseInsensitiveCompare(b.key) == .orderedAscending
        }
    }

    var body: some View {
        List {
            if cheats.isEmpty && unlabelled == 0 {
                Section { emptyState }
            }

            ForEach(grouped, id: \.0) { group, items in
                Section {
                    ForEach(items) { cheat in
                        cheatRow(cheat)
                    }
                } header: {
                    Text(group.isEmpty ? "Codes" : group)
                }
            }

            if unlabelled > 0 {
                Section {
                    Label("\(unlabelled) unnamed code\(unlabelled == 1 ? "" : "s")", systemImage: "questionmark.circle")
                        .foregroundStyle(.secondary)
                } footer: {
                    Text("These have no name in their .pnach file, so they cannot be listed or turned on one at a time. They activate whenever cheats are enabled for this game.")
                }
            }

            Section {
                Button {
                    showImporter = true
                } label: {
                    Label("Import .pnach File…", systemImage: "square.and.arrow.down")
                }
            } footer: {
                Text("Cheat files live in Documents/cheats and are matched to a game by its serial and CRC. You can also copy them there with the Files app.")
            }
        }
        .navigationTitle("Cheat Codes")
        .navigationBarTitleDisplayMode(.inline)
        .fileImporter(isPresented: $showImporter, allowedContentTypes: [.item]) { result in
            guard case .success(let url) = result else { return }
            guard url.pathExtension.lowercased() == "pnach" else {
                importMessage = "That is not a .pnach file."
                return
            }
            // Picked files live outside the sandbox until access is asked for.
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            if iPSX2Bridge.importPnach(atPath: url.path) {
                importMessage = "Imported \(url.lastPathComponent)."
                reload()
            } else {
                importMessage = "Could not copy \(url.lastPathComponent) into the cheats folder."
            }
        }
        .alert("Import", isPresented: Binding(
            get: { importMessage != nil },
            set: { if !$0 { importMessage = nil } }
        )) {
            Button("OK") {}
        } message: {
            Text(importMessage ?? "")
        }
        // Joins the edit session the parent screen opened rather than assuming it is still
        // open: the bridge counts how many screens are inside it, so this pairs off correctly
        // whichever order SwiftUI runs the parent's disappear and this view's appear in.
        .onAppear {
            _ = iPSX2Bridge.openGameSettings(forEntryPath: game.path)
            reload()
        }
        .onDisappear { iPSX2Bridge.closeGameSettings() }
    }

    @ViewBuilder
    private func cheatRow(_ cheat: GameCheat) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Toggle(isOn: Binding(
                get: { cheat.enabled },
                set: { on in
                    iPSX2Bridge.setCheat(cheat.name, enabled: on)
                    revision += 1
                    reload()
                }
            )) {
                Text(cheat.displayName.isEmpty ? cheat.name : cheat.displayName)
            }
            // Not disabled for the two names the global Widescreen / No-Interlacing switches own.
            // Those switches only ever add their name to the *patch* enable list, never the cheat
            // one, so a cheat that happens to share the name is toggled from here and from
            // nowhere else — greying it out would take away its only control.
            if !cheat.details.isEmpty {
                Text(cheat.details).font(.caption).foregroundStyle(.secondary)
            }
            if !cheat.author.isEmpty {
                Text("by \(cheat.author)").font(.caption2).foregroundStyle(.tertiary)
            }
        }
    }

    private var emptyState: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("No codes found for this game", systemImage: "key.slash")
                .font(.body)
            Text("Put a .pnach file for \(game.serial.isEmpty ? "this game" : game.serial) in Documents/cheats, or import one below. The filename must contain the game's serial — for example \(exampleFilename).")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 4)
    }

    private var exampleFilename: String {
        let serial = game.serial.isEmpty ? "SLPM-12345" : game.serial
        return "\(serial)_\(String(format: "%08X", game.crc)).pnach"
    }

    private func reload() {
        cheats = iPSX2Bridge.cheats(forEntryPath: game.path).map { GameCheat($0) }
        unlabelled = Int(iPSX2Bridge.unlabelledCheatCount(forEntryPath: game.path))
    }
}
