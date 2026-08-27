// GamepadSettingsView.swift — Virtual pad + controller mapping
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private struct PS2Button: Identifiable {
    let id: Int
    let name: String
}

private let ps2Buttons: [PS2Button] = [
    PS2Button(id: 0,  name: "D-Pad Up"),
    PS2Button(id: 1,  name: "D-Pad Down"),
    PS2Button(id: 2,  name: "D-Pad Left"),
    PS2Button(id: 3,  name: "D-Pad Right"),
    PS2Button(id: 4,  name: "Cross"),
    PS2Button(id: 5,  name: "Circle"),
    PS2Button(id: 6,  name: "Square"),
    PS2Button(id: 7,  name: "Triangle"),
    PS2Button(id: 8,  name: "L1"),
    PS2Button(id: 9,  name: "R1"),
    PS2Button(id: 12, name: "Start"),
    PS2Button(id: 13, name: "Select"),
    PS2Button(id: 14, name: "L3"),
    PS2Button(id: 15, name: "R3"),
]

// SDL_GamepadButton → display name (matches SDL3 enum order)
private func sdlButtonName(_ idx: Int) -> String {
    switch idx {
    case 0:  return "A / Cross"
    case 1:  return "B / Circle"
    case 2:  return "X / Square"
    case 3:  return "Y / Triangle"
    case 4:  return "Share / Back"
    case 5:  return "Guide / PS"
    case 6:  return "Options / Start"
    case 7:  return "L-Stick Press"
    case 8:  return "R-Stick Press"
    case 9:  return "L-Shoulder"
    case 10: return "R-Shoulder"
    case 11: return "D-Pad Up"
    case 12: return "D-Pad Down"
    case 13: return "D-Pad Left"
    case 14: return "D-Pad Right"
    case 15: return "Misc / Share"
    case 20: return "Touchpad"
    default: return "Button \(idx)"
    }
}

struct GamepadSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var capturingIndex: Int? = nil
    @State private var mappingVersion = 0
    @State private var pollTimer: Timer? = nil

    var body: some View {
        Form {
            Section {
                ForEach(ps2Buttons) { btn in
                    mappingRow(btn)
                }
            } header: {
                Text("Button Mapping")
            } footer: {
                Text("Tap a row, then press a button on your controller to assign it. L2/R2 are analog triggers (not remappable).")
            }

            Section {
                Button("Reset to Default") {
                    iPSX2Bridge.resetButtonMappings()
                    mappingVersion += 1
                }
                .foregroundStyle(.red)
            }
        }
        .navigationTitle("Game Controller")
        .navigationBarTitleDisplayMode(.inline)
        .onDisappear {
            stopCapture()
        }
    }

    @ViewBuilder
    private func mappingRow(_ btn: PS2Button) -> some View {
        let isCapturing = capturingIndex == btn.id
        let currentSDL = Int(iPSX2Bridge.getButtonMapping(Int32(btn.id)))

        Button {
            if isCapturing {
                stopCapture()
            } else {
                startCapture(for: btn.id)
            }
        } label: {
            HStack {
                // Left: assigned controller button (prominent)
                if isCapturing {
                    Text("Press a button...")
                        .font(.body)
                        .fontWeight(.medium)
                        .foregroundStyle(.orange)
                } else {
                    Text(sdlButtonName(currentSDL))
                        .font(.body)
                        .foregroundStyle(.primary)
                }
                Spacer()
                // Right: PS2 function name (secondary)
                Text(btn.name)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .id(mappingVersion)
        }
        .listRowBackground(isCapturing ? Color.orange.opacity(0.15) : nil)
    }

    private func startCapture(for ps2Index: Int) {
        capturingIndex = ps2Index
        iPSX2Bridge.startButtonCapture()
        pollTimer?.invalidate()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { _ in
            iPSX2Bridge.pollGamepadForCapture()
            let captured = iPSX2Bridge.capturedButton()
            if captured >= 0 {
                iPSX2Bridge.setButtonMapping(Int32(ps2Index), toSDLButton: captured)
                stopCapture()
                mappingVersion += 1
            }
        }
    }

    private func stopCapture() {
        pollTimer?.invalidate()
        pollTimer = nil
        capturingIndex = nil
        iPSX2Bridge.stopButtonCapture()
    }
}
