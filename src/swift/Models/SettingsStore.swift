// SettingsStore.swift — INI-backed settings for SwiftUI
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

/// [P51] OSD preset levels
enum OsdPreset: Int, CaseIterable {
    case off = 0
    case simple = 1    // FPS + CPU usage
    case detail = 2    // All except frame times graph
    case full = 3      // Everything

    var label: String {
        switch self {
        case .off: return "OFF"
        case .simple: return "Simple"
        case .detail: return "Detail"
        case .full: return "Full"
        }
    }
}

/// Which corner the performance overlay is drawn in. `EmuCore/GS OsdPerformancePos` accepts
/// nine positions, but the renderer only branches on TopLeft — everything else is right-aligned
/// at the same height — so offering more than two would be offering choices that look identical.
/// None is not a case here: hiding the overlay is what `OsdPreset.off` is for.
enum OsdCorner: Int, CaseIterable {
    case topLeft = 1    // OsdOverlayPos::TopLeft
    case topRight = 3   // OsdOverlayPos::TopRight

    var label: String {
        switch self {
        case .topLeft: return "Top Left"
        case .topRight: return "Top Right"
        }
    }
}

/// Per-component CPU engine. Mirrors VMManager.cpp parse_mode():
/// [EmuCore/CPU] EEMode/IOPMode/VU0Mode/VU1Mode = "interp" | "iv2" | "jit"
///
/// [JIT-OFF 2026-08-25] "jit" is deliberately NOT a case here. Code generation is not
/// permitted in an App Store app, so a jit selection was always degraded to iv2 at boot —
/// the settings screen said JIT while IV2 ran. The core still parses "jit" from the INI for
/// development, so this is a UI-level removal, not a capability removal; an INI that still
/// says "jit" reads back as iv2 here, which is what actually executes.
enum CpuMode: String, CaseIterable, Identifiable {
    case interpreter = "interp"
    case iv2 = "iv2"

    var id: String { rawValue }

    var label: String {
        switch self {
        case .interpreter: return "Interpreter"
        case .iv2: return "IV2"
        }
    }
}

@Observable
final class SettingsStore: @unchecked Sendable {
    static let shared = SettingsStore()

    // ── Emulator / CPU ──
    // Source of truth = [EmuCore/CPU] *Mode keys (string). Legacy CoreType /
    // Recompiler.Enable* are derived by VMManager at boot, so the UI writes Mode.
    var eeMode: CpuMode {
        didSet { iPSX2Bridge.setINIString("EmuCore/CPU", key: "EEMode", value: eeMode.rawValue) }
    }
    var iopMode: CpuMode {
        didSet { iPSX2Bridge.setINIString("EmuCore/CPU", key: "IOPMode", value: iopMode.rawValue) }
    }
    var vu0Mode: CpuMode {
        didSet { iPSX2Bridge.setINIString("EmuCore/CPU", key: "VU0Mode", value: vu0Mode.rawValue) }
    }
    var vu1Mode: CpuMode {
        didSet { iPSX2Bridge.setINIString("EmuCore/CPU", key: "VU1Mode", value: vu1Mode.rawValue) }
    }
    var fastBoot: Bool {
        didSet { iPSX2Bridge.setINIBool("GameISO", key: "FastBoot", value: fastBoot) }
    }
    // ── Audio (AUDIO) ──
    var audioVolume: Int {
        didSet { iPSX2Bridge.setAudioVolume(Int32(audioVolume)) }
    }
    var audioMuted: Bool {
        didSet { iPSX2Bridge.setAudioMuted(audioMuted) }
    }

    // ── Cheats / patches (PATCH) ──
    // Compatibility patches from GameDB are separate and always on; these are the user-facing
    // archives PCSX2 ships.
    // Each of these re-applies rather than waiting for a reboot: the core watches the patch
    // options and reloads the active patch list when they change, so a toggle here shows up in
    // the running game. Writing the INI alone left the switch looking done and doing nothing
    // until the next boot.
    var enableCheats: Bool {
        didSet {
            iPSX2Bridge.setINIBool("EmuCore", key: "EnableCheats", value: enableCheats)
            iPSX2Bridge.applySettingsNow()
        }
    }
    var enableWidescreenPatches: Bool {
        didSet {
            iPSX2Bridge.setINIBool("EmuCore", key: "EnableWideScreenPatches", value: enableWidescreenPatches)
            iPSX2Bridge.applySettingsNow()
        }
    }
    var enableNoInterlacingPatches: Bool {
        didSet {
            iPSX2Bridge.setINIBool("EmuCore", key: "EnableNoInterlacingPatches", value: enableNoInterlacingPatches)
            iPSX2Bridge.applySettingsNow()
        }
    }

    // ── Emulation speed (SPEED) ──
    // Percent rather than float: PCSX2 presents these as 100% / 200% / 50%, and whole numbers
    // keep the stepper from drifting on float round-trips.
    var normalSpeedPercent: Int {
        didSet { iPSX2Bridge.setNormalSpeedPercent(Int32(normalSpeedPercent)) }
    }
    var turboSpeedPercent: Int {
        didSet { iPSX2Bridge.setTurboSpeedPercent(Int32(turboSpeedPercent)) }
    }
    var slowMotionSpeedPercent: Int {
        didSet { iPSX2Bridge.setSlowMotionSpeedPercent(Int32(slowMotionSpeedPercent)) }
    }

    /// [JIT-OFF] Kept as stored state (the INI key still exists and a developer running a
    /// hand-set jit arm wants it) but no longer offered in the UI: CHECK_FASTMEM ANDs in
    /// EnableEE, which VMManager derives from EEMode==jit, so under all-iv2 it is always false.
    var fastmem: Bool {
        didSet { iPSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", value: fastmem) }
    }
    // Per-game VU1 kernel-HLE acceleration ([EmuCore] EnableGameHLE). On = native T&L for supported
    // games (FF10 / GTA VC / …). Device has no env vars, so this INI toggle is the only switch.
    var gameHLE: Bool {
        didSet { iPSX2Bridge.setINIBool("EmuCore", key: "EnableGameHLE", value: gameHLE) }
    }

    // ── Boot ──
    var fastCDVD: Bool {
        didSet { iPSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "fastCDVD", value: fastCDVD) }
    }

    // ── Advanced Speedhacks ──
    var eeCycleRate: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/Speedhacks", key: "EECycleRate", value: Int32(eeCycleRate)) }
    }
    var vu1Instant: Bool {
        didSet { iPSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "vu1Instant", value: vu1Instant) }
    }
    var waitLoop: Bool {
        didSet { iPSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "WaitLoop", value: waitLoop) }
    }
    var intcStat: Bool {
        didSet { iPSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "IntcStat", value: intcStat) }
    }

    // ── Graphics ──
    var renderer: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "Renderer", value: Int32(renderer)) }
    }
    var upscaleMultiplier: Float {
        didSet { iPSX2Bridge.setINIFloat("EmuCore/GS", key: "upscale_multiplier", value: upscaleMultiplier) }
    }
    var vsyncQueueSize: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "VsyncQueueSize", value: Int32(vsyncQueueSize)) }
    }
    var textureFiltering: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "filter", value: Int32(textureFiltering)) }
    }
    // [GFX] Anisotropic / trilinear filtering and shade boost — all live in the shared HW
    // renderer, so they work regardless of CPU engine.
    var maxAnisotropy: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "MaxAnisotropy", value: Int32(maxAnisotropy)) }
    }
    var triFilter: Int {   // TriFiltering: -1 Automatic, 0 Off, 1 PS2, 2 Forced
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "TriFilter", value: Int32(triFilter)) }
    }
    var shadeBoost: Bool {
        didSet { iPSX2Bridge.setINIBool("EmuCore/GS", key: "ShadeBoost", value: shadeBoost) }
    }
    var shadeBoostBrightness: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "ShadeBoost_Brightness", value: Int32(shadeBoostBrightness)) }
    }
    var shadeBoostContrast: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "ShadeBoost_Contrast", value: Int32(shadeBoostContrast)) }
    }
    var shadeBoostSaturation: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "ShadeBoost_Saturation", value: Int32(shadeBoostSaturation)) }
    }
    var shadeBoostGamma: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "ShadeBoost_Gamma", value: Int32(shadeBoostGamma)) }
    }

    var fxaa: Bool {
        didSet { iPSX2Bridge.setINIBool("EmuCore/GS", key: "fxaa", value: fxaa) }
    }
    var casMode: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "CASMode", value: Int32(casMode)) }
    }
    var casSharpness: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "CASSharpness", value: Int32(casSharpness)) }
    }
    var interlaceMode: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "deinterlace_mode", value: Int32(interlaceMode)) }
    }
    var aspectRatio: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "AspectRatio", value: Int32(aspectRatio)) }
    }
    var blendingAccuracy: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "accurate_blending_unit", value: Int32(blendingAccuracy)) }
    }
    var dithering: Int {
        didSet { iPSX2Bridge.setINIInt("EmuCore/GS", key: "dithering_ps2", value: Int32(dithering)) }
    }

    // ── OSD Overlay ──
    //
    // The preset is a starting point, not a lock: choosing one writes the whole set of item
    // flags, and each item can then be turned on or off individually. Every one of these writes
    // the INI and then re-applies, which is the only combination that both survives a restart
    // and shows up immediately — the overlay used to be poked straight into the live GS config,
    // so it was silently reverted the next time anything reloaded settings.
    var osdPreset: OsdPreset {
        didSet {
            iPSX2Bridge.setINIInt("iPSX2/UI", key: "OsdPreset", value: Int32(osdPreset.rawValue))
            applyOsdPreset(osdPreset)
        }
    }
    /// Position is stored separately from the preset so that turning the overlay off and on
    /// again comes back where the user put it.
    /// Stored under the app's own key rather than read back from OsdPerformancePos: hiding the
    /// overlay parks that key at None, so a corner read from it would come back as the default
    /// on the next launch and quietly forget a Top Left choice.
    var osdCorner: OsdCorner {
        didSet {
            iPSX2Bridge.setINIInt("iPSX2/UI", key: "OsdCorner", value: Int32(osdCorner.rawValue))
            writeOsdPosition()
        }
    }
    /// The game's own internal frame rate. Not the same as VPS, which is what actually reached
    /// the screen — a game running at 30 fps internally can still present at 60.
    var osdShowFPS: Bool {
        didSet { setOsdFlag("OsdShowFPS", osdShowFPS) }
    }
    var osdShowVPS: Bool {
        didSet { setOsdFlag("OsdShowVPS", osdShowVPS) }
    }
    var osdShowSpeed: Bool {
        didSet { setOsdFlag("OsdShowSpeed", osdShowSpeed) }
    }
    var osdShowCPU: Bool {
        didSet { setOsdFlag("OsdShowCPU", osdShowCPU) }
    }
    var osdShowGPU: Bool {
        didSet { setOsdFlag("OsdShowGPU", osdShowGPU) }
    }
    var osdShowResolution: Bool {
        didSet { setOsdFlag("OsdShowResolution", osdShowResolution) }
    }
    var osdShowGSStats: Bool {
        didSet { setOsdFlag("OsdShowGSStats", osdShowGSStats) }
    }
    var osdShowFrameTimes: Bool {
        didSet { setOsdFlag("OsdShowFrameTimes", osdShowFrameTimes) }
    }
    var osdShowVersion: Bool {
        didSet { setOsdFlag("OsdShowVersion", osdShowVersion) }
    }
    var osdShowHardwareInfo: Bool {
        didSet { setOsdFlag("OsdShowHardwareInfo", osdShowHardwareInfo) }
    }
    /// Fast-forward / pause / recording icons. Defaults ON in the core and is not part of any
    /// preset, so it can show up even with every other item off — which is why it is offered.
    var osdShowIndicators: Bool {
        didSet { setOsdFlag("OsdShowIndicators", osdShowIndicators) }
    }
    /// Text size multiplier for everything the OSD draws.
    var osdScale: Float {
        didSet {
            iPSX2Bridge.setINIFloat("EmuCore/GS", key: "OsdScale", value: osdScale)
            iPSX2Bridge.applySettingsNow()
        }
    }

    // ── Gamepad / UI ──
    var padOpacity: Float {
        didSet { iPSX2Bridge.setINIFloat("iPSX2/UI", key: "PadOpacity", value: padOpacity) }
    }
    var hapticFeedback: Bool {
        didSet { iPSX2Bridge.setINIBool("iPSX2/UI", key: "HapticFeedback", value: hapticFeedback) }
    }
    /// Whether the on-screen pad is drawn. Lives here rather than in the game view's own state
    /// so that hiding it survives leaving the game, and so it can be turned back on from
    /// Settings by someone who hid it and could not find the way back.
    var padVisible: Bool {
        didSet { iPSX2Bridge.setINIBool("iPSX2/UI", key: "PadVisible", value: padVisible) }
    }

    /// The preset to show, reconciled against the position. `OsdPerformancePos` is seeded to
    /// None on a config that does not mention it, so "nothing configured" reads as OFF here and
    /// the overlay really is hidden — the two can no longer drift apart.
    private static func storedOsdPreset() -> OsdPreset {
        let position = iPSX2Bridge.getINIInt("EmuCore/GS", key: "OsdPerformancePos",
                                             defaultValue: Int32(OsdCorner.topRight.rawValue))
        if position == 0 { return .off }   // OsdOverlayPos::None
        let stored = Int(iPSX2Bridge.getINIInt("iPSX2/UI", key: "OsdPreset", defaultValue: 0))
        // Visible, but the label says off (or is missing): report the mildest preset that is
        // honest about something being on screen.
        return OsdPreset(rawValue: stored).flatMap { $0 == .off ? nil : $0 } ?? .simple
    }

    /// Read a CPU mode key from INI, falling back to VMManager's parse_mode default.
    private static func cpuMode(_ key: String, _ def: CpuMode) -> CpuMode {
        let raw = iPSX2Bridge.getINIString("EmuCore/CPU", key: key, defaultValue: def.rawValue)
        // A legacy "jit" (or anything unknown) reports as iv2 because iv2 is what the core
        // actually runs here — showing the stored string instead would be the old lie.
        return CpuMode(rawValue: raw) ?? .iv2
    }

    // ── Init from INI ──
    private init() {
        audioVolume = Int(iPSX2Bridge.audioVolume())
        audioMuted = iPSX2Bridge.isAudioMuted()
        enableCheats = iPSX2Bridge.getINIBool("EmuCore", key: "EnableCheats", defaultValue: false)
        enableWidescreenPatches = iPSX2Bridge.getINIBool("EmuCore", key: "EnableWideScreenPatches", defaultValue: false)
        enableNoInterlacingPatches = iPSX2Bridge.getINIBool("EmuCore", key: "EnableNoInterlacingPatches", defaultValue: false)
        normalSpeedPercent = Int(iPSX2Bridge.normalSpeedPercent())
        turboSpeedPercent = Int(iPSX2Bridge.turboSpeedPercent())
        slowMotionSpeedPercent = Int(iPSX2Bridge.slowMotionSpeedPercent())
        // CPU — defaults match VMManager parse_mode (all iv2 since JIT-OFF)
        eeMode = SettingsStore.cpuMode("EEMode", .iv2)
        iopMode = SettingsStore.cpuMode("IOPMode", .iv2)
        vu0Mode = SettingsStore.cpuMode("VU0Mode", .iv2)
        vu1Mode = SettingsStore.cpuMode("VU1Mode", .iv2)
        fastBoot = iPSX2Bridge.getINIBool("GameISO", key: "FastBoot", defaultValue: false)
        fastmem = iPSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", defaultValue: false)
        gameHLE = iPSX2Bridge.getINIBool("EmuCore", key: "EnableGameHLE", defaultValue: true)
        // Boot
        fastCDVD = iPSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "fastCDVD", defaultValue: false)
        // Advanced Speedhacks
        eeCycleRate = Int(iPSX2Bridge.getINIInt("EmuCore/Speedhacks", key: "EECycleRate", defaultValue: 0))
        vu1Instant = iPSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vu1Instant", defaultValue: true)
        waitLoop = iPSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "WaitLoop", defaultValue: true)
        intcStat = iPSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "IntcStat", defaultValue: true)
        // Graphics
        renderer = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "Renderer", defaultValue: 17))
        upscaleMultiplier = iPSX2Bridge.getINIFloat("EmuCore/GS", key: "upscale_multiplier", defaultValue: 1.0)
        vsyncQueueSize = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "VsyncQueueSize", defaultValue: 8))
        maxAnisotropy = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "MaxAnisotropy", defaultValue: 0))
        triFilter = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "TriFilter", defaultValue: -1))
        shadeBoost = iPSX2Bridge.getINIBool("EmuCore/GS", key: "ShadeBoost", defaultValue: false)
        shadeBoostBrightness = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "ShadeBoost_Brightness", defaultValue: 50))
        shadeBoostContrast = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "ShadeBoost_Contrast", defaultValue: 50))
        shadeBoostSaturation = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "ShadeBoost_Saturation", defaultValue: 50))
        shadeBoostGamma = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "ShadeBoost_Gamma", defaultValue: 50))
        textureFiltering = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "filter", defaultValue: 2))
        fxaa = iPSX2Bridge.getINIBool("EmuCore/GS", key: "fxaa", defaultValue: false)
        casMode = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "CASMode", defaultValue: 0))
        casSharpness = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "CASSharpness", defaultValue: 50))
        interlaceMode = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "deinterlace_mode", defaultValue: 7))
        aspectRatio = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "AspectRatio", defaultValue: 0))
        blendingAccuracy = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "accurate_blending_unit", defaultValue: 1))
        dithering = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "dithering_ps2", defaultValue: 2))
        // OSD
        // The stored preset is a label; the position is what decides whether anything is drawn.
        // Reading the label alone let the two disagree — a config with no OSD keys at all reports
        // preset OFF while the core's own defaults put the overlay on screen.
        osdPreset = SettingsStore.storedOsdPreset()
        osdCorner = OsdCorner(rawValue: Int(iPSX2Bridge.getINIInt("iPSX2/UI", key: "OsdCorner",
                                                                 defaultValue: Int32(OsdCorner.topRight.rawValue)))) ?? .topRight
        osdShowFPS = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFPS", defaultValue: true)
        osdShowVPS = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVPS", defaultValue: true)
        osdShowSpeed = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowSpeed", defaultValue: true)
        osdShowCPU = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowCPU", defaultValue: true)
        osdShowGPU = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGPU", defaultValue: true)
        osdShowResolution = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowResolution", defaultValue: true)
        osdShowGSStats = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGSStats", defaultValue: false)
        osdShowFrameTimes = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFrameTimes", defaultValue: true)
        osdShowVersion = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVersion", defaultValue: true)
        osdShowHardwareInfo = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowHardwareInfo", defaultValue: true)
        osdShowIndicators = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowIndicators", defaultValue: true)
        osdScale = iPSX2Bridge.getINIFloat("EmuCore/GS", key: "OsdScale", defaultValue: 100.0)
        // UI
        padOpacity = iPSX2Bridge.getINIFloat("iPSX2/UI", key: "PadOpacity", defaultValue: 0.6)
        hapticFeedback = iPSX2Bridge.getINIBool("iPSX2/UI", key: "HapticFeedback", defaultValue: true)
        padVisible = iPSX2Bridge.getINIBool("iPSX2/UI", key: "PadVisible", defaultValue: true)
        // [P60] Force MTVU off (known buggy)
        iPSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "vuThread", value: false)
    }

    /// Reload ALL settings from INI (call on VM start/stop)
    func reload() {
        eeMode = SettingsStore.cpuMode("EEMode", .iv2)
        iopMode = SettingsStore.cpuMode("IOPMode", .iv2)
        vu0Mode = SettingsStore.cpuMode("VU0Mode", .iv2)
        vu1Mode = SettingsStore.cpuMode("VU1Mode", .iv2)
        fastBoot = iPSX2Bridge.getINIBool("GameISO", key: "FastBoot", defaultValue: false)
        fastmem = iPSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", defaultValue: false)
        gameHLE = iPSX2Bridge.getINIBool("EmuCore", key: "EnableGameHLE", defaultValue: true)
        fastCDVD = iPSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "fastCDVD", defaultValue: false)
        eeCycleRate = Int(iPSX2Bridge.getINIInt("EmuCore/Speedhacks", key: "EECycleRate", defaultValue: 0))
        vu1Instant = iPSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vu1Instant", defaultValue: true)
        waitLoop = iPSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "WaitLoop", defaultValue: true)
        intcStat = iPSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "IntcStat", defaultValue: true)
        renderer = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "Renderer", defaultValue: 17))
        upscaleMultiplier = iPSX2Bridge.getINIFloat("EmuCore/GS", key: "upscale_multiplier", defaultValue: 1.0)
        vsyncQueueSize = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "VsyncQueueSize", defaultValue: 8))
        maxAnisotropy = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "MaxAnisotropy", defaultValue: 0))
        triFilter = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "TriFilter", defaultValue: -1))
        shadeBoost = iPSX2Bridge.getINIBool("EmuCore/GS", key: "ShadeBoost", defaultValue: false)
        shadeBoostBrightness = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "ShadeBoost_Brightness", defaultValue: 50))
        shadeBoostContrast = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "ShadeBoost_Contrast", defaultValue: 50))
        shadeBoostSaturation = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "ShadeBoost_Saturation", defaultValue: 50))
        shadeBoostGamma = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "ShadeBoost_Gamma", defaultValue: 50))
        textureFiltering = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "filter", defaultValue: 2))
        fxaa = iPSX2Bridge.getINIBool("EmuCore/GS", key: "fxaa", defaultValue: false)
        casMode = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "CASMode", defaultValue: 0))
        casSharpness = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "CASSharpness", defaultValue: 50))
        interlaceMode = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "deinterlace_mode", defaultValue: 7))
        aspectRatio = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "AspectRatio", defaultValue: 0))
        blendingAccuracy = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "accurate_blending_unit", defaultValue: 1))
        dithering = Int(iPSX2Bridge.getINIInt("EmuCore/GS", key: "dithering_ps2", defaultValue: 2))
        // The stored preset is a label; the position is what decides whether anything is drawn.
        // Reading the label alone let the two disagree — a config with no OSD keys at all reports
        // preset OFF while the core's own defaults put the overlay on screen.
        osdPreset = SettingsStore.storedOsdPreset()
        osdCorner = OsdCorner(rawValue: Int(iPSX2Bridge.getINIInt("iPSX2/UI", key: "OsdCorner",
                                                                 defaultValue: Int32(OsdCorner.topRight.rawValue)))) ?? .topRight
        osdShowFPS = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFPS", defaultValue: true)
        osdShowVPS = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVPS", defaultValue: true)
        osdShowSpeed = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowSpeed", defaultValue: true)
        osdShowCPU = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowCPU", defaultValue: true)
        osdShowGPU = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGPU", defaultValue: true)
        osdShowResolution = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowResolution", defaultValue: true)
        osdShowGSStats = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGSStats", defaultValue: false)
        osdShowFrameTimes = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFrameTimes", defaultValue: true)
        osdShowVersion = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVersion", defaultValue: true)
        osdShowHardwareInfo = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowHardwareInfo", defaultValue: true)
        osdShowIndicators = iPSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowIndicators", defaultValue: true)
        osdScale = iPSX2Bridge.getINIFloat("EmuCore/GS", key: "OsdScale", defaultValue: 100.0)
        padOpacity = iPSX2Bridge.getINIFloat("iPSX2/UI", key: "PadOpacity", defaultValue: 0.6)
        hapticFeedback = iPSX2Bridge.getINIBool("iPSX2/UI", key: "HapticFeedback", defaultValue: true)
        padVisible = iPSX2Bridge.getINIBool("iPSX2/UI", key: "PadVisible", defaultValue: true)
    }

    /// Set while a preset is writing the whole item set, so nine individual writes do not each
    /// dispatch a full configuration reload to the CPU thread. The preset applies once at the end.
    @ObservationIgnored private var batchingOsdWrites = false

    /// One OSD item. Persists first, then re-reads — see the note on the OSD block above.
    private func setOsdFlag(_ key: String, _ value: Bool) {
        iPSX2Bridge.setINIBool("EmuCore/GS", key: key, value: value)
        if !batchingOsdWrites { iPSX2Bridge.applySettingsNow() }
    }

    /// The overlay is hidden by parking its position at None rather than by clearing every item,
    /// so the item selection is still there when it is turned back on.
    private func writeOsdPosition() {
        let pos = (osdPreset == .off) ? 0 : osdCorner.rawValue
        iPSX2Bridge.setINIInt("EmuCore/GS", key: "OsdPerformancePos", value: Int32(pos))
        iPSX2Bridge.applySettingsNow()
    }

    /// Applies a preset by writing every item flag. Assigning the properties runs their own
    /// didSet, so each one persists; the position write at the end is what actually shows or
    /// hides the overlay.
    ///
    /// OFF deliberately writes no item flags. Hiding the overlay is parking its position, not
    /// clearing its contents — clearing them would throw away a per-item selection the user had
    /// built up, and would make the in-game "turn it back on" restore the wrong thing.
    private func applyOsdPreset(_ preset: OsdPreset) {
        guard preset != .off else {
            writeOsdPosition()
            return
        }
        batchingOsdWrites = true
        defer { batchingOsdWrites = false }
        let isSimple = preset == .simple
        let isDetail = preset == .detail
        let isFull = preset == .full
        osdShowFPS = isSimple || isDetail || isFull
        osdShowSpeed = isDetail || isFull
        osdShowCPU = isSimple || isDetail || isFull
        osdShowResolution = isDetail || isFull
        osdShowFrameTimes = isFull
        osdShowVPS = isFull
        osdShowGPU = false
        osdShowGSStats = false
        osdShowVersion = false
        osdShowHardwareInfo = false
        writeOsdPosition()
    }

    /// Reset emulator settings to PC PCSX2 defaults
    func resetEmulatorDefaults() {
        eeMode = .iv2
        iopMode = .iv2
        vu0Mode = .iv2
        vu1Mode = .iv2
        fastBoot = false
        fastmem = false
        gameHLE = true
        fastCDVD = false
        eeCycleRate = 0
        vu1Instant = true       // PC PCSX2 recommended default
        waitLoop = true         // PC PCSX2 recommended default
        intcStat = true         // PC PCSX2 recommended default
    }

    /// Reset graphics settings to PC PCSX2 defaults
    func resetGraphicsDefaults() {
        renderer = 17           // Metal
        upscaleMultiplier = 1.0 // Native PS2
        vsyncQueueSize = 8
        textureFiltering = 2    // Bilinear (PS2)
        fxaa = false
        casMode = 0             // Disabled
        casSharpness = 50
        interlaceMode = 7       // Adaptive
        aspectRatio = 0         // Auto 4:3/3:2
        blendingAccuracy = 1    // Basic
        dithering = 2           // Scaled
    }
}
