// BIOSListView.swift — BIOS file list with default selection
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct BIOSListView: View {
    @State private var bioses: [String] = []
    @State private var defaultBIOS: String = ""

    var body: some View {
        NavigationStack {
            Group {
                if bioses.isEmpty {
                    emptyState
                } else {
                    List {
                        ForEach(bioses, id: \.self) { bios in
                            biosRow(bios)
                        }
                    }
                }
            }
            .navigationTitle("BIOS")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { loadBIOSes() } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
        }
        .onAppear { loadBIOSes() }
    }

    private func biosRow(_ bios: String) -> some View {
        Button {
            iPSX2Bridge.setDefaultBIOS(bios)
            defaultBIOS = bios
        } label: {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text(bios)
                        .font(.body)
                        .foregroundStyle(.primary)
                    Text(regionGuess(bios))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                if bios == defaultBIOS {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.blue)
                }
            }
        }
        .foregroundStyle(.primary)
    }

    // The old text named only Documents/bios/ and only .bin. Both were narrower than what the
    // code does: a BIOS-shaped file left in Documents/ is moved into bios/ for you, and .rom is
    // accepted alongside .bin. Dropping one in the root is the easier route — that is the folder
    // the Files app opens — so it is worth saying.
    private var emptyState: some View {
        ScrollView {
            VStack(spacing: 18) {
                Image(systemName: "cpu")
                    .font(.system(size: 48))
                    .foregroundStyle(.secondary)
                Text("No BIOS Found")
                    .font(.title2)
                    .fontWeight(.semibold)

                VStack(alignment: .leading, spacing: 12) {
                    folderHint(
                        icon: "folder.fill",
                        title: "Documents/bios/",
                        detail: "Where the BIOS is kept."
                    )
                    folderHint(
                        icon: "folder",
                        title: "Documents/",
                        detail: "A BIOS dropped here is moved into bios/ for you. Tap refresh after copying it over."
                    )
                }
                .frame(maxWidth: 420)

                VStack(spacing: 6) {
                    Text("Accepted files")
                        .font(.subheadline).fontWeight(.medium)
                    Text(".bin or .rom, between 1 MB and 50 MB")
                        .font(.caption).foregroundStyle(.secondary)
                    Text("Dump it from a PS2 you own.")
                        .font(.caption2).foregroundStyle(.tertiary)
                }

                Text("Copy files over with the Files app, or from a computer with Finder or iTunes File Sharing.")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 420)
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

    private func loadBIOSes() {
        bioses = iPSX2Bridge.availableBIOSes()
        defaultBIOS = iPSX2Bridge.defaultBIOSName()
    }

    private func regionGuess(_ name: String) -> String {
        let upper = name.uppercased()
        if upper.contains("JP") || upper.contains("JAPAN") || upper.contains("70000") || upper.contains("50000") {
            return "Japan"
        } else if upper.contains("US") || upper.contains("AMERICA") || upper.contains("30001") || upper.contains("39001") {
            return "North America"
        } else if upper.contains("EU") || upper.contains("EUROPE") || upper.contains("30004") || upper.contains("39004") {
            return "Europe"
        }
        return "Unknown Region"
    }
}
