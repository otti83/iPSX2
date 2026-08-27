// GraphicsSettingsView.swift — Renderer, upscale, filter, and display settings
// SPDX-License-Identifier: GPL-3.0+
//
// Explanatory text is in section footers rather than in rows of its own. A grey caption sitting
// inside a Form section is drawn as a list row, so it reads as something tappable; a footer is
// what iOS uses for exactly this and is what the sibling app does throughout.

import SwiftUI

struct GraphicsSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section {
                Picker("Renderer", selection: $settings.renderer) {
                    Text("Metal (Hardware)").tag(17)
                    Text("Software").tag(13)
                    Text("Null (No Output)").tag(11)
                }
            } header: {
                Text("Renderer")
            } footer: {
                Text("Metal is recommended. Software is slow but accurate. Null disables rendering, for testing. Takes effect on the next boot.")
            }

            Section {
                Picker("Internal Resolution", selection: $settings.upscaleMultiplier) {
                    Text("1x Native (512×448)").tag(Float(1.0))
                    Text("2x (1024×896)").tag(Float(2.0))
                    Text("3x (1536×1344)").tag(Float(3.0))
                    Text("4x (2048×1792)").tag(Float(4.0))
                    Text("5x (2560×2240)").tag(Float(5.0))
                    Text("6x (3072×2688)").tag(Float(6.0))
                    Text("8x (4096×3584)").tag(Float(8.0))
                }
            } header: {
                Text("Upscaling")
            } footer: {
                Text("Higher is sharper and costs a lot more GPU time. 4x and above suit A17 Pro and M-series devices. Takes effect on the next boot.")
            }

            Section {
                Picker("Texture Filtering", selection: $settings.textureFiltering) {
                    Text("Nearest (Pixelated)").tag(0)
                    Text("Bilinear (Forced)").tag(1)
                    Text("Bilinear (PS2 Default)").tag(2)
                    Text("Bilinear (Forced excl. Sprite)").tag(3)
                }
                Picker("Trilinear Filtering", selection: $settings.triFilter) {
                    Text("Automatic").tag(-1)
                    Text("Off").tag(0)
                    Text("PS2 (accurate)").tag(1)
                    Text("Forced").tag(2)
                }
                Picker("Anisotropic Filtering", selection: $settings.maxAnisotropy) {
                    Text("Off").tag(0)
                    Text("2x").tag(2)
                    Text("4x").tag(4)
                    Text("8x").tag(8)
                    Text("16x").tag(16)
                }
            } header: {
                Text("Filtering")
            } footer: {
                Text("Anisotropic filtering sharpens textures seen at an angle. It costs GPU time — lower it first if the frame rate suffers.")
            }

            Section {
                Toggle("FXAA", isOn: $settings.fxaa)
                Toggle("CAS Sharpening", isOn: Binding(
                    get: { settings.casMode > 0 },
                    set: { settings.casMode = $0 ? 1 : 0 }
                ))
                if settings.casMode > 0 {
                    HStack {
                        Text("Sharpness")
                        Slider(value: Binding(
                            get: { Float(settings.casSharpness) / 100.0 },
                            set: { settings.casSharpness = Int($0 * 100) }
                        ), in: 0...1)
                        Text("\(settings.casSharpness)%")
                            .font(.caption)
                            .frame(width: 40)
                    }
                }
            } header: {
                Text("Sharpening")
            } footer: {
                Text("FXAA smooths edges and may blur textures slightly. CAS sharpens the finished image.")
            }

            Section {
                Toggle("Shade Boost", isOn: $settings.shadeBoost)
                if settings.shadeBoost {
                    shadeSlider("Brightness", value: $settings.shadeBoostBrightness)
                    shadeSlider("Contrast", value: $settings.shadeBoostContrast)
                    shadeSlider("Saturation", value: $settings.shadeBoostSaturation)
                    shadeSlider("Gamma", value: $settings.shadeBoostGamma)
                }
            } header: {
                Text("Colour")
            } footer: {
                Text("Post-processing colour adjustment. 50 is neutral for every slider.")
            }

            Section {
                Picker("Deinterlace", selection: $settings.interlaceMode) {
                    Text("None").tag(0)
                    Text("Weave (TFF)").tag(1)
                    Text("Weave (BFF)").tag(2)
                    Text("Bob (TFF)").tag(3)
                    Text("Bob (BFF)").tag(4)
                    Text("Blend (TFF)").tag(5)
                    Text("Blend (BFF)").tag(6)
                    Text("Adaptive (Default)").tag(7)
                }
                Picker("Aspect Ratio", selection: $settings.aspectRatio) {
                    Text("Auto 4:3 / 3:2 (Default)").tag(0)
                    Text("4:3").tag(1)
                    Text("16:9 (Widescreen)").tag(2)
                    Text("Stretch to Window").tag(3)
                }
            } header: {
                Text("Display")
            } footer: {
                Text("Adaptive deinterlacing suits nearly every game. 16:9 only reshapes the picture — for a game to actually render widescreen it needs a widescreen patch, under Patches & Cheats.")
            }

            Section {
                Picker("Blending Accuracy", selection: $settings.blendingAccuracy) {
                    Text("Minimum (Fast)").tag(0)
                    Text("Basic (Default)").tag(1)
                    Text("Medium").tag(2)
                    Text("High").tag(3)
                    Text("Full (Slow)").tag(4)
                    Text("Ultra (Very Slow)").tag(5)
                }
                Picker("Dithering", selection: $settings.dithering) {
                    Text("Off").tag(0)
                    Text("Unscaled").tag(1)
                    Text("Scaled (Default)").tag(2)
                }
            } header: {
                Text("Quality")
            } footer: {
                Text("Higher blending accuracy fixes transparency and shadow artefacts at a real cost in performance.")
            }

            Section {
                Stepper("Queue Size: \(settings.vsyncQueueSize)", value: $settings.vsyncQueueSize, in: 2...16)
            } header: {
                Text("VSync")
            } footer: {
                Text("How many frames may be queued ahead. Higher reduces dropped frames and adds input latency.")
            }

            Section {
                Button(role: .destructive) {
                    settings.resetGraphicsDefaults()
                } label: {
                    Text("Reset Graphics to Defaults")
                }
            } footer: {
                Text("Only the settings on this screen. Per-game overrides in the Tweaks tab are untouched.")
            }
        }
        .navigationTitle("Graphics")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func shadeSlider(_ title: String, value: Binding<Int>) -> some View {
        HStack {
            Text(title).frame(width: 92, alignment: .leading)
            Slider(
                value: Binding(get: { Double(value.wrappedValue) }, set: { value.wrappedValue = Int($0) }),
                in: 0...100, step: 1
            )
            Text("\(value.wrappedValue)")
                .monospacedDigit()
                .frame(width: 34, alignment: .trailing)
                .foregroundStyle(.secondary)
        }
    }
}
