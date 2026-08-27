// OverlaySettingsView.swift — on-screen display
// SPDX-License-Identifier: GPL-3.0+
//
// The preset picks a starting set of items; every item can then be turned on or off on its own.
// This used to be preset-only, with the item list drawn as read-only checkmarks even though the
// underlying flags were already per-item.

import SwiftUI

struct OverlaySettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section {
                Picker("Preset", selection: $settings.osdPreset) {
                    ForEach(OsdPreset.allCases, id: \.self) { preset in
                        Text(preset.label).tag(preset)
                    }
                }
                .pickerStyle(.segmented)
            } header: {
                Text("Performance Overlay")
            } footer: {
                Text("A preset sets every item below at once. Change any of them afterwards and the preset stays as it is — it is a starting point, not a lock.")
            }

            if settings.osdPreset != .off {
                Section {
                    Picker("Position", selection: $settings.osdCorner) {
                        ForEach(OsdCorner.allCases, id: \.self) { corner in
                            Text(corner.label).tag(corner)
                        }
                    }
                    .pickerStyle(.segmented)
                } header: {
                    Text("Position")
                }

                Section {
                    Toggle("FPS", isOn: $settings.osdShowFPS)
                    Toggle("VPS", isOn: $settings.osdShowVPS)
                    Toggle("Speed %", isOn: $settings.osdShowSpeed)
                    Toggle("Resolution", isOn: $settings.osdShowResolution)
                } header: {
                    Text("Performance")
                } footer: {
                    Text("FPS is the rate the game itself is running at. VPS is the rate actually reaching the screen — a game running internally at 30 can still present at 60.")
                }

                Section {
                    Toggle("CPU Usage (EE / GS)", isOn: $settings.osdShowCPU)
                    Toggle("GPU Usage", isOn: $settings.osdShowGPU)
                    Toggle("Frame Times Graph", isOn: $settings.osdShowFrameTimes)
                    Toggle("Renderer Statistics", isOn: $settings.osdShowGSStats)
                } header: {
                    Text("Load")
                } footer: {
                    Text("The frame-time graph and renderer statistics are diagnostic — they cost a little performance themselves.")
                }

                Section {
                    Toggle("App Version", isOn: $settings.osdShowVersion)
                    Toggle("Device Info", isOn: $settings.osdShowHardwareInfo)
                } header: {
                    Text("System")
                }

                // Inside the `preset != .off` block on purpose: the icons are drawn by the same
                // routine as the rest of the overlay, which returns early when the overlay is
                // hidden. Offering this switch with the preset off would be offering a control
                // that cannot do anything.
                Section {
                    Toggle("Status Icons", isOn: $settings.osdShowIndicators)
                } footer: {
                    Text("Fast-forward, slow-motion and pause icons.")
                }
            }

            Section {
                HStack {
                    Text("Text Size")
                    Spacer()
                    Text("\(Int(settings.osdScale))%").foregroundStyle(.secondary)
                }
                Slider(value: $settings.osdScale, in: 50...300, step: 10)
            } footer: {
                Text("Applies to everything the overlay draws.")
            }
        }
        .navigationTitle("On-Screen Display")
        .navigationBarTitleDisplayMode(.inline)
    }
}
