// FileImportHandler.swift — Handle file import from Open-In / drag & drop
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

@Observable
final class FileImportHandler: @unchecked Sendable {
    static let shared = FileImportHandler()

    var lastImportMessage: String?
    var showImportAlert = false

    private static let biosExtensions: Set<String> = ["bin", "rom"]
    // Matches what the game scanner accepts, minus .bin which is decided by size below.
    private static let gameExtensions: Set<String> = ["iso", "chd", "img", "mdf", "cso", "zso", "gz", "elf"]
    private static let cheatExtensions: Set<String> = ["pnach"]
    // .bin files > 50MB are treated as game images, not BIOS
    private static let biosSizeThreshold: UInt64 = 50 * 1024 * 1024

    private init() {}

    func handleURL(_ url: URL) {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }

        let ext = url.pathExtension.lowercased()
        let fileName = url.lastPathComponent

        let docsPath = NSSearchPathForDirectoriesInDomains(.documentDirectory, .userDomainMask, true).first!

        // Determine destination
        let destDir: String
        let category: String

        if Self.gameExtensions.contains(ext) {
            destDir = (docsPath as NSString).appendingPathComponent("iso")
            category = "Game"
        } else if Self.cheatExtensions.contains(ext) {
            destDir = (docsPath as NSString).appendingPathComponent("cheats")
            category = "Cheat file"
        } else if Self.biosExtensions.contains(ext) {
            // Check file size to distinguish BIOS (.bin ~4MB) from game (.bin ~700MB)
            let attrs = try? FileManager.default.attributesOfItem(atPath: url.path)
            let size = attrs?[.size] as? UInt64 ?? 0
            if ext == "bin" && size > Self.biosSizeThreshold {
                destDir = (docsPath as NSString).appendingPathComponent("iso")
                category = "Game"
            } else {
                destDir = (docsPath as NSString).appendingPathComponent("bios")
                category = "BIOS"
            }
        } else {
            lastImportMessage = "Unsupported file type: .\(ext)"
            showImportAlert = true
            return
        }

        // Create directory if needed
        try? FileManager.default.createDirectory(atPath: destDir, withIntermediateDirectories: true)

        let destPath = (destDir as NSString).appendingPathComponent(fileName)

        // Copy file
        do {
            if FileManager.default.fileExists(atPath: destPath) {
                try FileManager.default.removeItem(atPath: destPath)
            }
            try FileManager.default.copyItem(at: url, to: URL(fileURLWithPath: destPath))
            lastImportMessage = "\(category) imported: \(fileName)"
        } catch {
            lastImportMessage = "Import failed: \(error.localizedDescription)"
        }
        showImportAlert = true
    }
}
