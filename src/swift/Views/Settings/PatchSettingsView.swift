// PatchSettingsView.swift — cheats and the bundled patch archives, globally
// SPDX-License-Identifier: GPL-3.0+
//
// [PATCH] Compatibility fixes that come from GameDB are applied automatically and are not
// listed here — these are the opt-in archives PCSX2 ships, which change how a game looks or
// plays rather than fixing it.
//
// Everything on this screen is global. Turning cheats on for one game, and choosing which of its
// codes run, is in the Tweaks tab.

import SwiftUI

struct PatchSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section {
                Toggle("Widescreen Patches", isOn: $settings.enableWidescreenPatches)
            } header: {
                Text("Display")
            } footer: {
                Text("Renders supported games at 16:9 instead of pillarboxed 4:3. Not every game has one, and a few misbehave with it.")
            }

            Section {
                Toggle("No-Interlacing Patches", isOn: $settings.enableNoInterlacingPatches)
            } footer: {
                Text("Removes the interlaced shimmer in supported games. May cost performance in a few.")
            }

            Section {
                Toggle("Enable Cheats", isOn: $settings.enableCheats)
            } header: {
                Text("Cheats")
            } footer: {
                Text("Turns cheat codes on for every game. Off by default — cheats can break saves and progression. To enable them for one game only, and to choose which codes run, use the Tweaks tab.")
            }
        }
        .navigationTitle("Patches & Cheats")
        .navigationBarTitleDisplayMode(.inline)
    }
}
