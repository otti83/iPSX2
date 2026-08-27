// ScanFolderStore.swift — folders outside the app that the game library scans
// SPDX-License-Identifier: GPL-3.0+
//
// A folder the user picks through the Files app is not readable just because we know its path.
// iOS grants access to the picked URL for as long as we hold a security scope open, and the
// only way to reopen that scope on a later launch is a bookmark saved at pick time. The path
// alone is worthless: the scanner would walk it and find nothing.
//
// So the bookmarks live here (in UserDefaults, which is frontend state), and the resolved paths
// are pushed into the core's scan list on every launch. The scope is opened before the first
// scan and deliberately never closed, because the C++ scanner reads with plain POSIX calls and
// has no idea any of this is happening.

import Foundation

@Observable
final class ScanFolderStore: @unchecked Sendable {
    static let shared = ScanFolderStore()

    private static let bookmarksKey = "gameScanFolderBookmarks"
    private static let recursiveKey = "gameScanFolderRecursive"

    /// Resolved path -> the URL whose scope we are holding open, so it stays retained.
    private var open: [String: URL] = [:]

    private init() {}

    /// Paths this store is currently holding access to.
    var accessiblePaths: Set<String> { Set(open.keys) }

    /// Re-opens every stored folder and registers it with the core. Call once at launch, before
    /// the first refresh. Bookmarks that no longer resolve (the folder was deleted, or the
    /// volume is not mounted) are dropped, so the list does not accumulate dead entries.
    func restoreAll() {
        let defaults = UserDefaults.standard
        let stored = defaults.array(forKey: Self.bookmarksKey) as? [Data] ?? []
        let recursive = defaults.dictionary(forKey: Self.recursiveKey) as? [String: Bool] ?? [:]
        var surviving: [Data] = []
        // Tracked explicitly rather than inferred from a count: a re-minted bookmark keeps the
        // count identical, and writing back only on a count change would throw the fresh data
        // away and re-do the same work on every launch until the old one stopped resolving.
        var changed = false

        for data in stored {
            var isStale = false
            guard let url = try? URL(resolvingBookmarkData: data,
                                     options: [],
                                     relativeTo: nil,
                                     bookmarkDataIsStale: &isStale),
                  url.startAccessingSecurityScopedResource()
            else {
                changed = true // dropped: it no longer resolves, or access was refused
                continue
            }

            let path = url.path
            open[path] = url
            iPSX2Bridge.addGameDirectory(path, recursive: recursive[path] ?? true)

            // A stale bookmark still resolved, but re-minting it now keeps it working after the
            // folder moves again.
            if isStale, let fresh = try? url.bookmarkData() {
                surviving.append(fresh)
                changed = true
            } else {
                surviving.append(data)
            }
        }

        if changed {
            defaults.set(surviving, forKey: Self.bookmarksKey)
            defaults.set(recursive.filter { open.keys.contains($0.key) }, forKey: Self.recursiveKey)
        }
    }

    /// Adds a folder the user just picked. Returns false when iOS refuses to mint a bookmark,
    /// which is the honest answer — without one the folder would work now and silently stop
    /// working on the next launch.
    @discardableResult
    func add(_ url: URL, recursive isRecursive: Bool) -> Bool {
        guard url.startAccessingSecurityScopedResource() else { return false }
        guard let data = try? url.bookmarkData() else {
            url.stopAccessingSecurityScopedResource()
            return false
        }

        let path = url.path
        let defaults = UserDefaults.standard
        var stored = defaults.array(forKey: Self.bookmarksKey) as? [Data] ?? []
        var recursive = defaults.dictionary(forKey: Self.recursiveKey) as? [String: Bool] ?? [:]

        if open[path] == nil { stored.append(data) }
        open[path] = url
        recursive[path] = isRecursive
        defaults.set(stored, forKey: Self.bookmarksKey)
        defaults.set(recursive, forKey: Self.recursiveKey)

        iPSX2Bridge.addGameDirectory(path, recursive: isRecursive)
        return true
    }

    /// Forgets a folder: drops the bookmark, closes the scope and takes it out of the scan list.
    /// Harmless to call for a folder this store never owned — that is how the folder list can
    /// route every removal through one path.
    func remove(path: String) {
        let defaults = UserDefaults.standard
        var recursive = defaults.dictionary(forKey: Self.recursiveKey) as? [String: Bool] ?? [:]

        if let url = open.removeValue(forKey: path) {
            url.stopAccessingSecurityScopedResource()
            // Bookmarks are opaque, so the one to drop is found by resolving each in turn.
            let stored = defaults.array(forKey: Self.bookmarksKey) as? [Data] ?? []
            let kept = stored.filter { data in
                var isStale = false
                let resolved = try? URL(resolvingBookmarkData: data, options: [],
                                        relativeTo: nil, bookmarkDataIsStale: &isStale)
                return resolved?.path != path
            }
            defaults.set(kept, forKey: Self.bookmarksKey)
        }

        recursive.removeValue(forKey: path)
        defaults.set(recursive, forKey: Self.recursiveKey)
        iPSX2Bridge.removeGameDirectory(path)
    }
}
