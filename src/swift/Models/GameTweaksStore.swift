// GameTweaksStore.swift — the settings a single game can override
// SPDX-License-Identifier: GPL-3.0+
//
// A per-game override file sits in a layer above the global INI, and the core reads through the
// stack, so a key present there wins and a key absent inherits. That third state — "not set" —
// is the whole point, and it is why this is a descriptor table rather than more properties on
// SettingsStore: every property there writes the global layer unconditionally in didSet and has
// no way to express "inherit".
//
// Only keys the core reads through the layer stack belong here. Two categories look like they
// belong and do not:
//   * anything the port reads straight off the base INI (the boot ISO's own fast-boot flag),
//     because an override would sit in the file and never be consulted;
//   * the frontend's own chrome under [iPSX2/UI], which the core never reads at all.
// A control that looks like it worked and did nothing is the failure this table exists to avoid.

import Foundation

enum TweakKind {
    case boolean
    case integer(range: ClosedRange<Int>)
    /// Fixed set of values, shown by name.
    case choice(values: [Int], labels: [String])
    case decimal(range: ClosedRange<Float>, step: Float)
}

struct TweakSetting: Identifiable {
    let section: String
    let key: String
    let title: String
    let help: String?
    let kind: TweakKind

    var id: String { "\(section)/\(key)" }
}

struct TweakGroup: Identifiable {
    let title: String
    let footer: String?
    let settings: [TweakSetting]

    var id: String { title }
}

enum GameTweaks {
    /// Kept deliberately short. Each of these is a key the core loads through the layered
    /// interface and that actually does something under the engine this app ships.
    static let groups: [TweakGroup] = [
        TweakGroup(
            title: "Speedhacks",
            footer: "The usual reason to change these for one game is that the game misbehaves with the global setting.",
            settings: [
                TweakSetting(section: "EmuCore/Speedhacks", key: "EECycleRate", title: "EE Cycle Rate",
                             help: "0 is standard. Negative underclocks the EE — faster here, but the game draws slightly less each frame. Positive overclocks it: slower here, but it fixes slowdown in a few games.",
                             kind: .integer(range: -3...3)),
                TweakSetting(section: "EmuCore/Speedhacks", key: "vu1Instant", title: "VU1 Instant",
                             help: nil, kind: .boolean),
                TweakSetting(section: "EmuCore/Speedhacks", key: "WaitLoop", title: "Wait Loop Detection",
                             help: nil, kind: .boolean),
                TweakSetting(section: "EmuCore/Speedhacks", key: "IntcStat", title: "INTC Stat Hack",
                             help: nil, kind: .boolean),
                TweakSetting(section: "EmuCore/Speedhacks", key: "fastCDVD", title: "Fast CDVD",
                             help: nil, kind: .boolean),
            ]
        ),
        TweakGroup(
            title: "Graphics",
            footer: "Useful when one game needs more accuracy than the global setting gives it, or can afford less.",
            settings: [
                TweakSetting(section: "EmuCore/GS", key: "upscale_multiplier", title: "Internal Resolution",
                             help: "1x is native PS2. Higher is sharper and costs GPU time.",
                             kind: .decimal(range: 1.0...3.0, step: 1.0)),
                TweakSetting(section: "EmuCore/GS", key: "AspectRatio", title: "Aspect Ratio",
                             help: nil,
                             kind: .choice(values: [0, 1, 2], labels: ["Auto 4:3 / 3:2", "4:3", "16:9"])),
                TweakSetting(section: "EmuCore/GS", key: "deinterlace_mode", title: "Deinterlacing",
                             help: nil,
                             kind: .choice(values: [0, 1, 7], labels: ["Off", "Weave", "Adaptive"])),
                TweakSetting(section: "EmuCore/GS", key: "accurate_blending_unit", title: "Blending Accuracy",
                             help: "Higher fixes transparency and shadow artefacts at a cost.",
                             kind: .choice(values: [0, 1, 2, 3], labels: ["Minimum", "Basic", "Medium", "High"])),
                TweakSetting(section: "EmuCore/GS", key: "filter", title: "Texture Filtering",
                             help: nil,
                             kind: .choice(values: [0, 1, 2], labels: ["Nearest", "Bilinear (Forced)", "Bilinear (PS2)"])),
            ]
        ),
        TweakGroup(
            title: "Speed",
            footer: "The target this game is paced to. Lower it for a game that runs too fast to play.",
            settings: [
                TweakSetting(section: "Framerate", key: "NominalScalar", title: "Normal Speed",
                             help: nil, kind: .decimal(range: 0.1...4.0, step: 0.1)),
            ]
        ),
        TweakGroup(
            title: "Cheats",
            footer: "Turns cheat codes on for this game without turning them on globally. Individual codes are chosen below.",
            settings: [
                TweakSetting(section: "EmuCore", key: "EnableCheats", title: "Enable Cheats",
                             help: nil, kind: .boolean),
            ]
        ),
    ]

    /// Deliberately absent, and why — each of these looks like it belongs and would be a control
    /// that silently does nothing:
    ///
    /// * **CPU engine (EEMode / IOPMode / VU0Mode / VU1Mode).** VMManager picks the engine with
    ///   `Host::GetBaseStringSettingValue`, which reads the *base* layer only, so a per-game
    ///   value is never consulted.
    /// * **Fast Boot (`EmuCore/EnableFastBoot`).** The core only falls back to it when the boot
    ///   parameters leave fast-boot unset, and this port always sets it explicitly from
    ///   `[GameISO] FastBoot`. The global switch in Settings writes that other key.
    /// * **Widescreen / No-Interlacing patch switches.** The core strips these from per-game
    ///   files (`ClearInvalidPerGameConfiguration`).
    /// * **Anything under `[iPSX2/UI]`.** Frontend chrome; the core never reads it.
}
