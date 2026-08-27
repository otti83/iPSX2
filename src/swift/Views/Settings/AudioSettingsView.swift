// AudioSettingsView.swift — volume, mute
// SPDX-License-Identifier: GPL-3.0+
//
// [AUDIO] There was no volume control of any kind before this. Volume applies live rather
// than on next boot — a slider that does nothing until you restart is worse than no slider.

import SwiftUI

struct AudioSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section {
                Toggle("Mute", isOn: $settings.audioMuted)
                HStack {
                    Image(systemName: "speaker.fill").foregroundStyle(.secondary)
                    Slider(
                        value: Binding(
                            get: { Double(settings.audioVolume) },
                            set: { settings.audioVolume = Int($0) }
                        ),
                        in: 0...100, step: 1
                    )
                    .disabled(settings.audioMuted)
                    Image(systemName: "speaker.wave.3.fill").foregroundStyle(.secondary)
                    Text("\(settings.audioVolume)%")
                        .monospacedDigit()
                        .frame(width: 46, alignment: .trailing)
                        .foregroundStyle(.secondary)
                }
                Text("Applies immediately. The device volume buttons control the system level on top of this.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Volume")
            }
        }
        .navigationTitle("Audio")
        .navigationBarTitleDisplayMode(.inline)
    }
}
