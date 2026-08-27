// EmulatorSettingsView.swift — EE/IOP/VU/boot/speedhack settings
// SPDX-License-Identifier: GPL-3.0+
//
// Global defaults. Anything that only one game needs belongs in the Tweaks tab, which layers
// over these.

import SwiftUI

struct EmulatorSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section {
                cpuModePicker("EE Core", selection: $settings.eeMode)
                cpuModePicker("IOP", selection: $settings.iopMode)
                cpuModePicker("VU0", selection: $settings.vu0Mode)
                cpuModePicker("VU1", selection: $settings.vu1Mode)
            } header: {
                Text("CPU Engine")
            } footer: {
                Text("IV2 is the default and what this app runs on: no code generation, which is what App Store rules allow. Interpreter is slower but the most faithful — switch a component to it if a game misbehaves. Takes effect on the next boot.")
            }

            // [SPEED] The multipliers only; the mode switch itself lives in the in-game menu,
            // because that is where you use it.
            Section {
                speedStepper("Normal", value: $settings.normalSpeedPercent)
                speedStepper("Fast Forward", value: $settings.turboSpeedPercent)
                speedStepper("Slow Motion", value: $settings.slowMotionSpeedPercent)
            } header: {
                Text("Emulation Speed")
            } footer: {
                Text("Fast Forward and Slow Motion are switched on from the in-game menu. Normal is the target the emulator paces itself to — lower it if a game runs too fast to play.")
            }

            Section {
                Toggle("Fast Boot", isOn: $settings.fastBoot)
            } header: {
                Text("Boot")
            } footer: {
                Text("Skips the BIOS intro. A few games need it to run in order to initialise — missing 3D graphics is the usual sign.")
            }

            Section {
                Toggle("Game HLE Acceleration", isOn: $settings.gameHLE)
            } header: {
                Text("Game HLE")
            } footer: {
                Text("Speeds up supported games (FF X, GTA Vice City, …) with native geometry kernels. Leave it on unless a game shows graphical problems. Takes effect on the next boot.")
            }

            Section {
                Stepper("EE Cycle Rate: \(settings.eeCycleRate)", value: $settings.eeCycleRate, in: -3...3)
                Toggle("Fast CDVD", isOn: $settings.fastCDVD)
                Toggle("VU1 Instant", isOn: $settings.vu1Instant)
                Toggle("Wait Loop Detection", isOn: $settings.waitLoop)
                Toggle("INTC Stat Hack", isOn: $settings.intcStat)
            } header: {
                Text("Speedhacks")
            } footer: {
                Text("Cycle Rate 0 is standard. Negative underclocks the EE: faster here, but the game draws slightly less each frame (−1 is roughly +4% speed for about 1% fewer draws; −2 and below drop around 7%). Positive overclocks it — slower here, but it fixes slowdown in a few games. The four switches are PCSX2's recommended defaults; turn one off only for a game that needs it. All of these take effect on the next boot.")
            }

            Section {
                Button(role: .destructive) {
                    settings.resetEmulatorDefaults()
                } label: {
                    Text("Reset Emulator to Defaults")
                }
            } footer: {
                Text("Only the settings on this screen. Per-game overrides in the Tweaks tab are untouched.")
            }
        }
        .navigationTitle("Emulation")
        .navigationBarTitleDisplayMode(.inline)
    }

    @ViewBuilder
    private func speedStepper(_ title: String, value: Binding<Int>) -> some View {
        Stepper(value: value, in: 10...400, step: 10) {
            HStack {
                Text(title)
                Spacer()
                Text("\(value.wrappedValue)%").foregroundStyle(.secondary)
            }
        }
    }

    private func cpuModePicker(_ title: String, selection: Binding<CpuMode>) -> some View {
        Picker(title, selection: selection) {
            ForEach(CpuMode.allCases) { mode in
                Text(mode.label).tag(mode)
            }
        }
    }
}
