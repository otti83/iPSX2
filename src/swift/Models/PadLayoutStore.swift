// PadLayoutStore.swift — INI-backed virtual pad layout positions
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct PadGroupPosition {
    var x: CGFloat
    var y: CGFloat
    var scale: CGFloat
}

@Observable
final class PadLayoutStore: @unchecked Sendable {
    static let shared = PadLayoutStore()

    static let groupIDs = ["dpad", "action", "l1", "l2", "r1", "r2", "lstick", "rstick", "select", "start"]

    var portrait: [String: PadGroupPosition] = [:]
    var landscape: [String: PadGroupPosition] = [:]

    // MARK: - Default positions (derived from current hardcoded layout)

    // Portrait: relative to controller area (0.0-1.0)
    // U002: action x adjusted from 0.88/0.92 to 0.85/0.88 to prevent ○ button clipping
    static let defaultPortrait: [String: PadGroupPosition] = [
        "l2":     PadGroupPosition(x: 0.16, y: 0.06, scale: 1.0),
        "l1":     PadGroupPosition(x: 0.16, y: 0.14, scale: 1.0),
        "r2":     PadGroupPosition(x: 0.84, y: 0.06, scale: 1.0),
        "r1":     PadGroupPosition(x: 0.84, y: 0.14, scale: 1.0),
        "select": PadGroupPosition(x: 0.43, y: 0.20, scale: 1.0),
        "start":  PadGroupPosition(x: 0.57, y: 0.20, scale: 1.0),
        "dpad":   PadGroupPosition(x: 0.16, y: 0.48, scale: 1.0),
        "action": PadGroupPosition(x: 0.82, y: 0.44, scale: 1.0),
        "lstick": PadGroupPosition(x: 0.28, y: 0.78, scale: 1.0),
        "rstick": PadGroupPosition(x: 0.72, y: 0.78, scale: 1.0),
    ]

    // Landscape: relative to full screen — positions kept well inside safe area
    // to avoid clipping on notch/Dynamic Island devices
    static let defaultLandscape: [String: PadGroupPosition] = [
        "dpad":   PadGroupPosition(x: 0.14, y: 0.72, scale: 1.0),
        "action": PadGroupPosition(x: 0.84, y: 0.72, scale: 1.0),
        "l2":     PadGroupPosition(x: 0.14, y: 0.22, scale: 1.0),
        "l1":     PadGroupPosition(x: 0.14, y: 0.34, scale: 1.0),
        "r2":     PadGroupPosition(x: 0.86, y: 0.22, scale: 1.0),
        "r1":     PadGroupPosition(x: 0.86, y: 0.34, scale: 1.0),
        "select": PadGroupPosition(x: 0.43, y: 0.90, scale: 1.0),
        "start":  PadGroupPosition(x: 0.57, y: 0.90, scale: 1.0),
        "lstick": PadGroupPosition(x: 0.26, y: 0.86, scale: 1.0),
        "rstick": PadGroupPosition(x: 0.74, y: 0.86, scale: 1.0),
    ]

    private init() {
        portrait = Self.defaultPortrait
        landscape = Self.defaultLandscape
        load()
    }

    func position(for id: String, landscape isLandscape: Bool) -> PadGroupPosition {
        let dict = isLandscape ? landscape : portrait
        let defaults = isLandscape ? Self.defaultLandscape : Self.defaultPortrait
        return dict[id] ?? defaults[id] ?? PadGroupPosition(x: 0.5, y: 0.5, scale: 1.0)
    }

    // MARK: - INI persistence

    func save() {
        for id in Self.groupIDs {
            if let pos = portrait[id] {
                iPSX2Bridge.setINIFloat("iPSX2/PadLayout/Portrait", key: "\(id)_x", value: Float(pos.x))
                iPSX2Bridge.setINIFloat("iPSX2/PadLayout/Portrait", key: "\(id)_y", value: Float(pos.y))
                iPSX2Bridge.setINIFloat("iPSX2/PadLayout/Portrait", key: "\(id)_scale", value: Float(pos.scale))
            }
            if let pos = landscape[id] {
                iPSX2Bridge.setINIFloat("iPSX2/PadLayout/Landscape", key: "\(id)_x", value: Float(pos.x))
                iPSX2Bridge.setINIFloat("iPSX2/PadLayout/Landscape", key: "\(id)_y", value: Float(pos.y))
                iPSX2Bridge.setINIFloat("iPSX2/PadLayout/Landscape", key: "\(id)_scale", value: Float(pos.scale))
            }
        }
    }

    func load() {
        for id in Self.groupIDs {
            // Portrait
            let px = iPSX2Bridge.getINIFloat("iPSX2/PadLayout/Portrait", key: "\(id)_x", defaultValue: -1)
            if px >= 0 {
                let py = iPSX2Bridge.getINIFloat("iPSX2/PadLayout/Portrait", key: "\(id)_y", defaultValue: 0.5)
                let ps = iPSX2Bridge.getINIFloat("iPSX2/PadLayout/Portrait", key: "\(id)_scale", defaultValue: 1.0)
                portrait[id] = PadGroupPosition(x: CGFloat(px), y: CGFloat(py), scale: CGFloat(ps))
            }
            // Landscape
            let lx = iPSX2Bridge.getINIFloat("iPSX2/PadLayout/Landscape", key: "\(id)_x", defaultValue: -1)
            if lx >= 0 {
                let ly = iPSX2Bridge.getINIFloat("iPSX2/PadLayout/Landscape", key: "\(id)_y", defaultValue: 0.5)
                let ls = iPSX2Bridge.getINIFloat("iPSX2/PadLayout/Landscape", key: "\(id)_scale", defaultValue: 1.0)
                landscape[id] = PadGroupPosition(x: CGFloat(lx), y: CGFloat(ly), scale: CGFloat(ls))
            }
        }
    }

    func resetPortrait() {
        portrait = Self.defaultPortrait
        save()
    }

    func resetLandscape() {
        landscape = Self.defaultLandscape
        save()
    }

    func reset(isLandscape: Bool) {
        if isLandscape { resetLandscape() } else { resetPortrait() }
    }
}
