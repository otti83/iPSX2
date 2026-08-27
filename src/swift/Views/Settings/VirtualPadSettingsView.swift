// VirtualPadSettingsView.swift — Virtual pad visibility, opacity, haptic, layout editing
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct VirtualPadSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var showLayoutEditor = false

    var body: some View {
        Form {
            Section {
                // Also in the in-game menu, but that is unreachable if the pad is what you
                // hid — and until this was stored it was forgotten on every launch anyway.
                Toggle("Show Virtual Pad", isOn: $settings.padVisible)
            } footer: {
                Text("Hide it when you are playing with a connected controller. This is the same switch as the one in the in-game menu.")
            }

            Section {
                VStack(alignment: .leading) {
                    Text("Opacity: \(Int(settings.padOpacity * 100))%")
                    Slider(value: $settings.padOpacity, in: 0.1...1.0, step: 0.05)
                }
            } header: {
                Text("Appearance")
            }

            Section {
                Toggle("Haptic Feedback", isOn: $settings.hapticFeedback)
            } header: {
                Text("Feedback")
            }

            Section {
                Button {
                    showLayoutEditor = true
                } label: {
                    Label("Edit Layout", systemImage: "square.resize")
                }
            } header: {
                Text("Layout")
            } footer: {
                Text("Drag buttons to reposition them. Pinch to resize.")
            }
        }
        .navigationTitle("Virtual Pad")
        .navigationBarTitleDisplayMode(.inline)
        .fullScreenCover(isPresented: $showLayoutEditor) {
            PadLayoutEditView()
        }
    }
}
