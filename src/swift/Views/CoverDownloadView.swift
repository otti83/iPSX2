// CoverDownloadView.swift — bulk cover art download
// SPDX-License-Identifier: GPL-3.0+
//
// [COVER] Deliberately ships NO default URL. Upstream PCSX2's cover downloader works the same
// way: the user supplies a template, because the covers live on third-party hosts and which
// one to use is not ours to decide. The screen's job is to explain the placeholders and make
// the template easy to keep.
//
// The presets below are the one concession to that: nothing is fetched until the user taps one,
// and the stored template still starts empty. They exist because finding a template that
// actually resolves is the hard part — the sources are keyed in ways our placeholders have to
// match exactly, and each entry here was measured against the live host rather than guessed.

import SwiftUI

struct CoverDownloadView: View {
    @Environment(\.dismiss) private var dismiss
    @AppStorage("coverUrlTemplate") private var urlTemplate = ""
    @AppStorage("coverUseSerial") private var useSerial = true
    @State private var running = false
    @State private var progress = 0
    @State private var outcome: Outcome? = nil
    @State private var timer: Timer? = nil
    @FocusState private var templateFocused: Bool

    private static let placeholders = [
        "${serial}", "${title}", "${titleen}", "${title_safe}", "${titleen_safe}", "${filetitle}",
        "${region}",
    ]

    // What a finished run actually did. The core cannot say this on its own: it reports the
    // already-covered library ("no URLs to download enumerated") as a failure and a run where
    // every single request 404'd as a success, so the two most ordinary outcomes came out
    // exactly backwards. The bridge counts instead, and this reads the counts.
    private struct Outcome {
        let success: Bool
        let saved: Int
        let missing: Int
        let cancelled: Bool

        var message: String {
            if cancelled {
                return saved > 0 ? "Stopped after \(saved) cover\(saved == 1 ? "" : "s")" : "Stopped"
            }
            if missing == 0 { return "Every game already has a cover" }
            if saved == 0 {
                // DownloadCovers still returns true when every request 404s: it ran fine, the
                // source simply has none of these games. A false means it never got that far.
                return success ? "No cover matched this template"
                               : "Could not download — check the template and your connection"
            }
            if saved < missing { return "Downloaded \(saved) of \(missing) — this source has nothing for the rest" }
            return "Downloaded \(saved) of \(missing)"
        }

        var wentWell: Bool { !cancelled && (missing == 0 || saved > 0) }
    }

    private struct Preset: Identifiable {
        let id: String
        let name: String
        let detail: String
        let template: String
    }

    // Measured against the live hosts, not assumed:
    //  - ps2-covers is keyed on the uppercase hyphenated serial, which is exactly what ${serial}
    //    already produces; 10,220 of GameDB's 12,838 serials have a file there (79.6%), and 8,712
    //    of the 9,817 retail SLPM/SLES/SLUS/SLPS discs (88.7%).
    //  - its 3d directory is PNG only — .jpg 404s on every entry — and covers slightly more
    //    serials (83.4%) at roughly 1 MB per image instead of ~100 KB.
    //  - libretro-thumbnails is keyed on the Redump name, so it needs the English title and the
    //    region as a word; it resolves for about half a library, and the spaces must be written
    //    %20 (see the note under the template field).
    private static let presets: [Preset] = [
        Preset(
            id: "ps2covers",
            name: "PS2 Covers",
            detail: "Front covers, by serial — the widest coverage",
            template: "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/${serial}.jpg"
        ),
        Preset(
            id: "ps2covers3d",
            name: "PS2 Covers (3D box)",
            detail: "The same collection rendered as a box — about 1 MB each",
            template: "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/3d/${serial}.png"
        ),
        Preset(
            id: "libretro",
            name: "Libretro Thumbnails",
            detail: "By English title and region — misses anything named differently",
            template: "https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation_2/master/Named_Boxarts/${titleen_safe}%20(${region}).png"
        ),
    ]

    private var templateIsUsable: Bool {
        Self.placeholders.contains { urlTemplate.contains($0) }
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("https://example.com/${serial}.jpg", text: $urlTemplate, axis: .vertical)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .focused($templateFocused)
                        .disabled(running)
                    Text("iPSX2 does not ship a source — enter the one you want to use. The template must contain at least one placeholder:")
                        .font(.caption).foregroundStyle(.secondary)
                    VStack(alignment: .leading, spacing: 3) {
                        placeholderRow("${serial}", "SLPS-25627")
                        placeholderRow("${titleen}", "English title from GameDB")
                        placeholderRow("${titleen_safe}", "English title, & * / : ` < > ? \\ | → _")
                        placeholderRow("${title}", "Title as shown in the list")
                        placeholderRow("${title_safe}", "Same, with those characters replaced")
                        placeholderRow("${filetitle}", "The file's name")
                        placeholderRow("${region}", "Japan / USA / Europe / Korea …")
                    }
                    .font(.caption2)
                    Text("Collections that store covers by name usually want ${titleen_safe} — most are keyed on the English title with those characters swapped for underscores, and a Japanese-titled game would never match ${title}.")
                        .font(.caption).foregroundStyle(.secondary)
                    Text("Only what a placeholder stands for gets escaped, so write a space in the rest of the template as %20 — one literal space and the whole address is re-escaped and misses.")
                        .font(.caption).foregroundStyle(.secondary)
                } header: {
                    Text("URL Template")
                }

                Section {
                    ForEach(Self.presets) { preset in
                        presetRow(preset)
                    }
                } header: {
                    Text("Presets")
                } footer: {
                    Text("Public collections run by other people — iPSX2 hosts no images and asks for nothing until you pick one. Tapping a preset only fills in the template above.")
                }

                Section {
                    Toggle("Name files by serial", isOn: $useSerial).disabled(running)
                    Text("On, covers are saved as SLPS-25627.jpg and follow the disc even if you rename the file. Off, they are saved under the game's title.")
                        .font(.caption).foregroundStyle(.secondary)
                }

                Section {
                    if running {
                        VStack(alignment: .leading, spacing: 8) {
                            ProgressView(value: Double(progress), total: 100)
                            Text("\(progress)%").font(.caption).foregroundStyle(.secondary)
                        }
                        Button(role: .destructive) {
                            iPSX2Bridge.cancelCoverDownload()
                        } label: { Text("Cancel") }
                    } else {
                        Button {
                            start()
                        } label: {
                            Label("Download Covers", systemImage: "arrow.down.circle")
                        }
                        .disabled(!templateIsUsable)
                    }
                    if let outcome {
                        Label(
                            outcome.message,
                            systemImage: outcome.wentWell ? "checkmark.circle" : "exclamationmark.triangle"
                        )
                        .font(.caption)
                        .foregroundStyle(outcome.wentWell ? .green : .orange)
                    }
                } footer: {
                    Text("Existing covers are kept; only games without one are fetched. Saved to \(iPSX2Bridge.coversDirectory()).")
                }
            }
            .navigationTitle("Download Covers")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") { dismiss() }.disabled(running)
                }
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("iPSX2CoverDownloadDidFinish"))) { note in
            running = false
            stopPolling()
            let info = note.userInfo
            outcome = Outcome(
                success: (info?["success"] as? NSNumber)?.boolValue ?? false,
                saved: (info?["saved"] as? NSNumber)?.intValue ?? 0,
                missing: (info?["missing"] as? NSNumber)?.intValue ?? 0,
                cancelled: (info?["cancelled"] as? NSNumber)?.boolValue ?? false
            )
        }
        .onDisappear { stopPolling() }
    }

    private func presetRow(_ preset: Preset) -> some View {
        Button {
            // Leaving the field focused would keep the keyboard over the list and, on a
            // multi-line TextField, hide the change it just made.
            templateFocused = false
            urlTemplate = preset.template
        } label: {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text(preset.name)
                        .font(.body)
                        .foregroundStyle(.primary)
                    Text(preset.detail)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                if urlTemplate == preset.template {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.blue)
                }
            }
        }
        .foregroundStyle(.primary)
        .disabled(running)
    }

    private func placeholderRow(_ token: String, _ meaning: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text(token).monospaced().foregroundStyle(.primary)
            Text(meaning).foregroundStyle(.secondary)
        }
    }

    private func start() {
        outcome = nil
        progress = 0
        running = true
        iPSX2Bridge.downloadCovers(withTemplate: urlTemplate, useSerial: useSerial)
        // The core reports progress through a counter rather than a callback, so poll it.
        timer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: true) { _ in
            progress = Int(iPSX2Bridge.coverDownloadProgress())
        }
    }

    private func stopPolling() {
        timer?.invalidate()
        timer = nil
    }
}
