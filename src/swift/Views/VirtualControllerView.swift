// VirtualControllerView.swift — PS2 DualShock2 virtual controller
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

// U005: Singleton haptic generator — prepared once, reused for all button presses
@MainActor
enum HapticManager {
    static let medium: UIImpactFeedbackGenerator = {
        let g = UIImpactFeedbackGenerator(style: .medium)
        g.prepare()
        return g
    }()
    static let light: UIImpactFeedbackGenerator = {
        let g = UIImpactFeedbackGenerator(style: .light)
        g.prepare()
        return g
    }()
}

struct VirtualControllerView: View {
    @State private var settings = SettingsStore.shared
    @State private var layout = PadLayoutStore.shared
    var isLandscape: Bool = false

    // A004: Scale buttons based on screen width (baseline: iPhone 15 = 393pt width)
    private func deviceScale(_ geo: GeometryProxy) -> CGFloat {
        let baseWidth: CGFloat = 393
        let w = isLandscape ? max(geo.size.width, geo.size.height) : min(geo.size.width, geo.size.height)
        return max(0.7, min(1.4, w / baseWidth))
    }

    var body: some View {
        GeometryReader { geo in
            if isLandscape {
                landscapeLayout(w: geo.size.width, h: geo.size.height)
                    .opacity(Double(settings.padOpacity))
            } else {
                portraitLayout(w: geo.size.width, h: geo.size.height)
            }
        }
    }

    private func pos(_ id: String, landscape: Bool) -> PadGroupPosition {
        layout.position(for: id, landscape: landscape)
    }

    // MARK: - Landscape: overlay on game screen
    @ViewBuilder
    func landscapeLayout(w: CGFloat, h: CGFloat) -> some View {
        ZStack {
            DPadView(size: 110)
                .scaleEffect(pos("dpad", landscape: true).scale)
                .position(x: pos("dpad", landscape: true).x * w, y: pos("dpad", landscape: true).y * h)
            ActionButtonsView(size: 42)
                .scaleEffect(pos("action", landscape: true).scale)
                .position(x: pos("action", landscape: true).x * w, y: pos("action", landscape: true).y * h)
            PadBtn(label: "L2", w: 130, h: 44, btn: .L2)
                .scaleEffect(pos("l2", landscape: true).scale)
                .position(x: pos("l2", landscape: true).x * w, y: pos("l2", landscape: true).y * h)
            PadBtn(label: "L1", w: 120, h: 32, btn: .L1)
                .scaleEffect(pos("l1", landscape: true).scale)
                .position(x: pos("l1", landscape: true).x * w, y: pos("l1", landscape: true).y * h)
            PadBtn(label: "R2", w: 130, h: 44, btn: .R2)
                .scaleEffect(pos("r2", landscape: true).scale)
                .position(x: pos("r2", landscape: true).x * w, y: pos("r2", landscape: true).y * h)
            PadBtn(label: "R1", w: 120, h: 32, btn: .R1)
                .scaleEffect(pos("r1", landscape: true).scale)
                .position(x: pos("r1", landscape: true).x * w, y: pos("r1", landscape: true).y * h)
            PadBtn(label: "SEL", w: 40, h: 22, btn: .select)
                .scaleEffect(pos("select", landscape: true).scale)
                .position(x: pos("select", landscape: true).x * w, y: pos("select", landscape: true).y * h)
            PadBtn(label: "START", w: 48, h: 22, btn: .start)
                .scaleEffect(pos("start", landscape: true).scale)
                .position(x: pos("start", landscape: true).x * w, y: pos("start", landscape: true).y * h)
            StickView(isLeft: true)
                .scaleEffect(pos("lstick", landscape: true).scale)
                .position(x: pos("lstick", landscape: true).x * w, y: pos("lstick", landscape: true).y * h)
            StickView(isLeft: false)
                .scaleEffect(pos("rstick", landscape: true).scale)
                .position(x: pos("rstick", landscape: true).x * w, y: pos("rstick", landscape: true).y * h)
        }
    }

    // MARK: - Portrait: controller fills its given area
    @ViewBuilder
    func portraitLayout(w: CGFloat, h: CGFloat) -> some View {
        ZStack {
            Color(white: 0.10).opacity(Double(settings.padOpacity))  // A002: apply opacity to background too

            GeometryReader { cGeo in
                let cW = cGeo.size.width
                let cH = cGeo.size.height

                PadBtn(label: "L2", w: 110, h: 40, btn: .L2)
                    .scaleEffect(pos("l2", landscape: false).scale)
                    .position(x: pos("l2", landscape: false).x * cW, y: pos("l2", landscape: false).y * cH)
                PadBtn(label: "L1", w: 100, h: 30, btn: .L1)
                    .scaleEffect(pos("l1", landscape: false).scale)
                    .position(x: pos("l1", landscape: false).x * cW, y: pos("l1", landscape: false).y * cH)
                PadBtn(label: "R2", w: 110, h: 40, btn: .R2)
                    .scaleEffect(pos("r2", landscape: false).scale)
                    .position(x: pos("r2", landscape: false).x * cW, y: pos("r2", landscape: false).y * cH)
                PadBtn(label: "R1", w: 100, h: 30, btn: .R1)
                    .scaleEffect(pos("r1", landscape: false).scale)
                    .position(x: pos("r1", landscape: false).x * cW, y: pos("r1", landscape: false).y * cH)
                PadBtn(label: "SEL", w: 42, h: 22, btn: .select)
                    .scaleEffect(pos("select", landscape: false).scale)
                    .position(x: pos("select", landscape: false).x * cW, y: pos("select", landscape: false).y * cH)
                PadBtn(label: "START", w: 48, h: 22, btn: .start)
                    .scaleEffect(pos("start", landscape: false).scale)
                    .position(x: pos("start", landscape: false).x * cW, y: pos("start", landscape: false).y * cH)
                DPadView(size: 100)
                    .scaleEffect(pos("dpad", landscape: false).scale)
                    .position(x: pos("dpad", landscape: false).x * cW, y: pos("dpad", landscape: false).y * cH)
                ActionButtonsView(size: 42)
                    .scaleEffect(pos("action", landscape: false).scale)
                    .position(x: pos("action", landscape: false).x * cW, y: pos("action", landscape: false).y * cH)
                StickView(isLeft: true)
                    .scaleEffect(pos("lstick", landscape: false).scale)
                    .position(x: pos("lstick", landscape: false).x * cW, y: pos("lstick", landscape: false).y * cH)
                StickView(isLeft: false)
                    .scaleEffect(pos("rstick", landscape: false).scale)
                    .position(x: pos("rstick", landscape: false).x * cW, y: pos("rstick", landscape: false).y * cH)
            }
            .opacity(Double(settings.padOpacity))
        }
    }
}

// MARK: - D-Pad
struct DPadView: View {
    let size: CGFloat
    var body: some View {
        let a = size * 0.30
        ZStack {
            PadBtn(label: "▲", w: a, h: a, btn: .up).offset(y: -a)
            PadBtn(label: "▼", w: a, h: a, btn: .down).offset(y: a)
            PadBtn(label: "◀", w: a, h: a, btn: .left).offset(x: -a)
            PadBtn(label: "▶", w: a, h: a, btn: .right).offset(x: a)
        }
    }
}

// MARK: - Action Buttons
struct ActionButtonsView: View {
    let size: CGFloat
    var body: some View {
        let sp = size * 1.1
        ZStack {
            PSBtn(sym: "△", clr: .green, sz: size, btn: .triangle).offset(y: -sp)
            PSBtn(sym: "✕", clr: .blue, sz: size, btn: .cross).offset(y: sp)
            PSBtn(sym: "□", clr: .pink, sz: size, btn: .square).offset(x: -sp)
            PSBtn(sym: "○", clr: .red, sz: size, btn: .circle).offset(x: sp)
        }
    }
}

struct PSBtn: View {
    let sym: String; let clr: Color; let sz: CGFloat; let btn: iPSX2PadButton
    @State private var on = false
    var body: some View {
        Text(sym)
            .font(.system(size: sz * 0.42, weight: .bold))
            .foregroundStyle(on ? .white : clr)
            .frame(width: sz, height: sz)
            .background(Circle().fill(on ? clr.opacity(0.6) : .black.opacity(0.25))
                .stroke(clr.opacity(on ? 1.0 : 0.5), lineWidth: on ? 2.5 : 1.5))
            .scaleEffect(on ? 0.88 : 1.0)
            .animation(.easeOut(duration: 0.06), value: on)
            .contentShape(Circle())
            .simultaneousGesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in guard !on else { return }; on = true
                    EmulatorBridge.shared.setPadButton(btn, pressed: true)
                    if SettingsStore.shared.hapticFeedback {
                        HapticManager.medium.impactOccurred()
                    }
                }
                .onEnded { _ in on = false; EmulatorBridge.shared.setPadButton(btn, pressed: false) })
    }
}

struct PadBtn: View {
    let label: String; let w: CGFloat; let h: CGFloat; let btn: iPSX2PadButton
    @State private var on = false
    var body: some View {
        Text(label)
            .font(.system(size: min(w, h) * 0.38, weight: .semibold))
            .foregroundStyle(on ? .black : .white)
            .frame(width: w, height: h)
            .background(RoundedRectangle(cornerRadius: 7)
                .fill(on ? .white.opacity(0.7) : .black.opacity(0.22))
                .stroke(.white.opacity(on ? 0.8 : 0.25), lineWidth: 1))
            .scaleEffect(on ? 0.9 : 1.0)
            .animation(.easeOut(duration: 0.06), value: on)
            .contentShape(Rectangle())
            .simultaneousGesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in guard !on else { return }; on = true
                    EmulatorBridge.shared.setPadButton(btn, pressed: true)
                    if SettingsStore.shared.hapticFeedback {
                        HapticManager.medium.impactOccurred()
                    }
                }
                .onEnded { _ in on = false; EmulatorBridge.shared.setPadButton(btn, pressed: false) })
    }
}

// MARK: - Analog Stick with L3/R3 tap
struct StickView: View {
    let isLeft: Bool
    let sz: CGFloat = 68; let knob: CGFloat = 30
    @State private var off: CGSize = .zero
    @State private var isDragging = false

    var body: some View {
        ZStack {
            Circle().fill(.black.opacity(0.18)).stroke(.white.opacity(0.18), lineWidth: 1).frame(width: sz)
            Circle().fill(.white.opacity(0.35)).frame(width: knob).offset(off)
            // L3/R3 label
            Text(isLeft ? "L3" : "R3")
                .font(.system(size: 9, weight: .medium))
                .foregroundStyle(.white.opacity(0.3))
                .offset(y: sz / 2 + 8)
        }
        .contentShape(Circle())
        .simultaneousGesture(DragGesture(minimumDistance: 0)
            .onChanged { v in
                let maxR = (sz - knob) / 2
                let dist = hypot(v.translation.width, v.translation.height)
                if dist > 4 {
                    isDragging = true
                    let d = min(dist, maxR)
                    let a = atan2(v.translation.height, v.translation.width)
                    off = CGSize(width: cos(a) * d, height: sin(a) * d)
                    let nx = Float(cos(a) * d / maxR); let ny = Float(sin(a) * d / maxR)
                    isLeft ? EmulatorBridge.shared.setLeftStick(x: nx, y: ny)
                           : EmulatorBridge.shared.setRightStick(x: nx, y: ny)
                }
            }
            .onEnded { _ in
                if !isDragging {
                    // Tap (no significant drag) → L3/R3 press
                    let btn: iPSX2PadButton = isLeft ? .L3 : .R3
                    EmulatorBridge.shared.setPadButton(btn, pressed: true)
                    if SettingsStore.shared.hapticFeedback {
                        HapticManager.light.impactOccurred()
                    }
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                        EmulatorBridge.shared.setPadButton(btn, pressed: false)
                    }
                } else {
                    // Drag ended → reset stick
                    withAnimation(.spring(duration: 0.12)) { off = .zero }
                    isLeft ? EmulatorBridge.shared.setLeftStick(x: 0, y: 0)
                           : EmulatorBridge.shared.setRightStick(x: 0, y: 0)
                }
                isDragging = false
            })
    }
}
