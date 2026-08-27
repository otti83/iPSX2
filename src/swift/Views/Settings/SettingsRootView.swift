// SettingsRootView.swift — Settings navigation root
// SPDX-License-Identifier: GPL-3.0+
//
// Named groups rather than one flat list, matching iPSX3. The first row is "Emulation" and not
// "Emulator" on purpose: a group called Emulator containing a row called Emulator reads as a
// mistake.

import SwiftUI

struct SettingsRootView: View {
    var body: some View {
        List {
            Section("Emulation") {
                NavigationLink {
                    EmulatorSettingsView()
                } label: {
                    Label("Emulation", systemImage: "cpu")
                }
                NavigationLink {
                    GraphicsSettingsView()
                } label: {
                    Label("Graphics", systemImage: "paintbrush")
                }
                NavigationLink {
                    AudioSettingsView()
                } label: {
                    Label("Audio", systemImage: "speaker.wave.2")
                }
            }

            Section {
                NavigationLink {
                    GameFoldersView()
                } label: {
                    Label("Game Folders", systemImage: "folder")
                }
                NavigationLink {
                    PatchSettingsView()
                } label: {
                    Label("Patches & Cheats", systemImage: "bandage")
                }
            } header: {
                Text("Games")
            } footer: {
                Text("Settings for one game at a time live in the Tweaks tab.")
            }

            Section("Display") {
                NavigationLink {
                    OverlaySettingsView()
                } label: {
                    Label("On-Screen Display", systemImage: "chart.bar.xaxis")
                }
            }

            Section("Controls") {
                NavigationLink {
                    GamepadSettingsView()
                } label: {
                    Label("Game Controller", systemImage: "gamecontroller")
                }
                NavigationLink {
                    VirtualPadSettingsView()
                } label: {
                    Label("Virtual Pad", systemImage: "hand.draw")
                }
            }

            Section("About") {
                NavigationLink {
                    HelpView()
                } label: {
                    Label("Help", systemImage: "questionmark.circle")
                }
                NavigationLink {
                    LicenseView()
                } label: {
                    Label("Licenses & Credits", systemImage: "doc.text")
                }
                HStack {
                    Text("Version")
                    Spacer()
                    Text(iPSX2Bridge.buildVersion())
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }
            }
        }
        .navigationTitle("Settings")
        .navigationBarTitleDisplayMode(.inline)
    }
}
