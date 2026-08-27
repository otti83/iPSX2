// GameFoldersView.swift — which folders the game library scans
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

struct ScanFolder: Identifiable, Hashable {
    let path: String
    let recursive: Bool
    let builtIn: Bool
    let exists: Bool

    var id: String { path }
    var displayName: String { (path as NSString).lastPathComponent }

    init(_ d: [String: Any]) {
        path      = d["path"] as? String ?? ""
        recursive = (d["recursive"] as? NSNumber)?.boolValue ?? false
        builtIn   = (d["builtIn"] as? NSNumber)?.boolValue ?? false
        exists    = (d["exists"] as? NSNumber)?.boolValue ?? false
    }
}

struct GameFoldersView: View {
    @State private var folders: [ScanFolder] = []
    @State private var showPicker = false
    @State private var addRecursively = true
    @State private var pickerFailed = false

    private var builtIn: [ScanFolder] { folders.filter(\.builtIn) }
    private var added: [ScanFolder] { folders.filter { !$0.builtIn } }

    var body: some View {
        List {
            Section {
                ForEach(builtIn) { folder in
                    row(folder)
                }
            } header: {
                Text("Always Scanned")
            } footer: {
                Text("These are the app's own folders. They are checked every time the library refreshes and cannot be removed.")
            }

            Section {
                if added.isEmpty {
                    Text("No folders added.")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(added) { folder in
                        row(folder)
                    }
                    .onDelete { offsets in
                        for index in offsets {
                            ScanFolderStore.shared.remove(path: added[index].path)
                        }
                        reloadAndRefresh()
                    }
                }
            } header: {
                Text("Your Folders")
            } footer: {
                Text("Add any folder you keep games in — on this device, on iCloud Drive, or on connected storage. Swipe a row to remove it. Removing a folder only stops it being scanned; nothing is deleted.")
            }

            Section {
                Toggle("Include Subfolders", isOn: $addRecursively)
                Button {
                    showPicker = true
                } label: {
                    Label("Add Folder…", systemImage: "folder.badge.plus")
                }
            } footer: {
                Text("Applies to the next folder you add.")
            }
        }
        .navigationTitle("Game Folders")
        .navigationBarTitleDisplayMode(.inline)
        .fileImporter(isPresented: $showPicker, allowedContentTypes: [.folder]) { result in
            guard case .success(let url) = result else { return }
            // A folder picked here is outside our sandbox. The store keeps a bookmark so the
            // access survives a relaunch; without one it would work now and quietly stop later,
            // which is worse than refusing.
            if ScanFolderStore.shared.add(url, recursive: addRecursively) {
                reloadAndRefresh()
            } else {
                pickerFailed = true
            }
        }
        .alert("Could Not Add Folder", isPresented: $pickerFailed) {
            Button("OK") {}
        } message: {
            Text("iOS did not grant lasting access to that folder. Try one under this device's own storage or iCloud Drive.")
        }
        .onAppear(perform: reload)
    }

    private func row(_ folder: ScanFolder) -> some View {
        HStack(spacing: 12) {
            Image(systemName: folder.recursive ? "folder.fill" : "folder")
                .foregroundStyle(folder.exists ? .blue : .secondary)
            VStack(alignment: .leading, spacing: 2) {
                Text(folder.displayName.isEmpty ? folder.path : folder.displayName)
                    .lineLimit(1)
                Text(folder.recursive ? "Including subfolders" : "This folder only")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if !folder.exists {
                    Text("Not found")
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
            }
        }
    }

    private func reload() {
        folders = iPSX2Bridge.gameDirectoryEntries().map { ScanFolder($0) }
    }

    /// A folder change is only real once the library has looked at it, so rescan rather than
    /// leaving the user to find the refresh button themselves.
    private func reloadAndRefresh() {
        reload()
        guard !iPSX2Bridge.isVMRunning() else { return }
        iPSX2Bridge.refreshGameList(false)
    }
}
