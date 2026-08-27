// PadLayoutEditView.swift — Drag-to-edit virtual pad layout
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct PadLayoutEditView: View {
    @State private var layout = PadLayoutStore.shared
    @State private var editLandscape = false
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        Group {
            if editLandscape {
                GeometryReader { geo in
                    ZStack {
                        Color(white: 0.08)

                        // Safe zone guide: shows where buttons won't be clipped by notch/home indicator
                        // Uses a fixed 5% margin on each side as a universal safe guide
                        let margin: CGFloat = 0.05
                        let guideRect = CGRect(
                            x: geo.size.width * margin,
                            y: geo.size.height * margin,
                            width: geo.size.width * (1 - margin * 2),
                            height: geo.size.height * (1 - margin * 2)
                        )
                        Rectangle()
                            .stroke(style: StrokeStyle(lineWidth: 1, dash: [6, 4]))
                            .foregroundStyle(.yellow.opacity(0.3))
                            .frame(width: guideRect.width, height: guideRect.height)
                            .position(x: guideRect.midX, y: guideRect.midY)

                        Text("Safe Zone")
                            .font(.caption2)
                            .foregroundStyle(.yellow.opacity(0.25))
                            .position(x: guideRect.midX, y: guideRect.minY + 12)

                        editorContent(areaW: geo.size.width, areaH: geo.size.height)

                        VStack {
                            floatingToolbar
                            Spacer()
                            Button("Reset to Default") {
                                layout.reset(isLandscape: true)
                            }
                            .font(.caption)
                            .foregroundStyle(.red.opacity(0.7))
                            .padding(.bottom, 6)
                        }
                    }
                    .ignoresSafeArea()
                }
            } else {
                GeometryReader { geo in
                    ZStack {
                        VStack(spacing: 0) {
                            ZStack {
                                Color.black
                                Text("Game Screen")
                                    .foregroundStyle(.white.opacity(0.3))
                                    .font(.caption)
                            }
                            .frame(height: geo.size.height / 2)

                            ZStack {
                                Color(white: 0.10)

                                // Safe zone guide for portrait pad area
                                let padH = geo.size.height / 2
                                let pm: CGFloat = 0.04
                                Rectangle()
                                    .stroke(style: StrokeStyle(lineWidth: 1, dash: [6, 4]))
                                    .foregroundStyle(.yellow.opacity(0.3))
                                    .padding(.horizontal, geo.size.width * pm)
                                    .padding(.vertical, padH * pm)

                                Text("Safe Zone")
                                    .font(.caption2)
                                    .foregroundStyle(.yellow.opacity(0.25))
                                    .position(x: geo.size.width / 2, y: 14)

                                editorPortraitContent(areaW: geo.size.width, areaH: padH)
                            }
                            .frame(height: geo.size.height / 2)
                        }

                        VStack {
                            HStack {
                                Button("Cancel") { dismiss() }
                                    .padding(8)
                                    .background(.black.opacity(0.5), in: Capsule())
                                Spacer()
                                Picker("", selection: $editLandscape) {
                                    Text("Portrait").tag(false)
                                    Text("Landscape").tag(true)
                                }
                                .pickerStyle(.segmented)
                                .frame(width: 200)
                                .background(.black.opacity(0.3), in: RoundedRectangle(cornerRadius: 8))
                                Spacer()
                                Button("Save") {
                                    layout.save()
                                    dismiss()
                                }
                                .fontWeight(.semibold)
                                .padding(8)
                                .background(.black.opacity(0.5), in: Capsule())
                            }
                            .padding(.horizontal)
                            .padding(.top, 4)
                            Spacer()
                            Button("Reset to Default") {
                                layout.reset(isLandscape: false)
                            }
                            .font(.caption)
                            .foregroundStyle(.red.opacity(0.7))
                            .padding(.bottom, 6)
                        }
                    }
                }
            }
        }
        .background(Color(white: 0.08))
        .navigationBarHidden(true)
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
    }

    private var floatingToolbar: some View {
        HStack {
            Button("Cancel") { dismiss() }
                .padding(8)
                .background(.black.opacity(0.5), in: Capsule())
            Spacer()
            Picker("", selection: $editLandscape) {
                Text("Portrait").tag(false)
                Text("Landscape").tag(true)
            }
            .pickerStyle(.segmented)
            .frame(width: 200)
            .background(.black.opacity(0.3), in: RoundedRectangle(cornerRadius: 8))
            Spacer()
            Button("Save") {
                layout.save()
                dismiss()
            }
            .fontWeight(.semibold)
            .padding(8)
            .background(.black.opacity(0.5), in: Capsule())
        }
        .padding(.horizontal)
        .padding(.top, 4)
    }

    @ViewBuilder
    private func editorContent(areaW: CGFloat, areaH: CGFloat) -> some View {
        ZStack {
            ForEach(PadLayoutStore.groupIDs, id: \.self) { id in
                DraggableGroup(id: id, areaW: areaW, areaH: areaH, isLandscape: editLandscape)
            }
        }
    }

    @ViewBuilder
    private func editorPortraitContent(areaW: CGFloat, areaH: CGFloat) -> some View {
        ZStack {
            ForEach(PadLayoutStore.groupIDs, id: \.self) { id in
                DraggableGroup(id: id, areaW: areaW, areaH: areaH, isLandscape: false)
            }
        }
    }
}

// MARK: - Draggable group widget
private struct DraggableGroup: View {
    let id: String
    let areaW: CGFloat
    let areaH: CGFloat
    let isLandscape: Bool

    @State private var layout = PadLayoutStore.shared
    @State private var dragOffset: CGSize = .zero
    @State private var currentScale: CGFloat = 1.0

    private var pos: PadGroupPosition {
        layout.position(for: id, landscape: isLandscape)
    }

    private var currentX: CGFloat { pos.x * areaW + dragOffset.width }
    private var currentY: CGFloat { pos.y * areaH + dragOffset.height }
    private var scaledSize: CGSize {
        let s = pos.scale * currentScale
        return CGSize(width: groupSize.width * s, height: groupSize.height * s)
    }

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 8)
                .fill(.white.opacity(0.05))
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(.blue.opacity(0.4), lineWidth: 1.5)
                )
                .frame(width: scaledSize.width + 16, height: scaledSize.height + 16)

            groupContent
                .scaleEffect(pos.scale * currentScale)
                .allowsHitTesting(false)

            Text(id.uppercased())
                .font(.system(size: 9, weight: .bold))
                .foregroundStyle(.blue.opacity(0.7))
                .offset(y: -(scaledSize.height / 2 + 12))
        }
        .position(x: currentX, y: currentY)
        .opacity(0.8)
        .gesture(
            DragGesture()
                .onChanged { v in dragOffset = v.translation }
                .onEnded { v in
                    let newX = (pos.x * areaW + v.translation.width) / areaW
                    let newY = (pos.y * areaH + v.translation.height) / areaH
                    updatePosition(x: newX.clamped(0, 1), y: newY.clamped(0, 1))
                    dragOffset = .zero
                }
        )
        .simultaneousGesture(
            MagnifyGesture()
                .onChanged { v in currentScale = v.magnification }
                .onEnded { v in
                    let newScale = (pos.scale * v.magnification).clamped(0.5, 2.0)
                    updateScale(newScale)
                    currentScale = 1.0
                }
        )
    }

    private func updatePosition(x: CGFloat, y: CGFloat) {
        var p = pos
        p.x = x
        p.y = y
        if isLandscape {
            layout.landscape[id] = p
        } else {
            layout.portrait[id] = p
        }
    }

    private func updateScale(_ scale: CGFloat) {
        var p = pos
        p.scale = scale
        if isLandscape {
            layout.landscape[id] = p
        } else {
            layout.portrait[id] = p
        }
    }

    private var groupSize: CGSize {
        switch id {
        case "dpad":   return CGSize(width: 120, height: 120)
        case "action": return CGSize(width: 120, height: 120)
        case "l1":     return CGSize(width: 110, height: 36)
        case "l2":     return CGSize(width: 120, height: 48)
        case "r1":     return CGSize(width: 110, height: 36)
        case "r2":     return CGSize(width: 120, height: 48)
        case "lstick", "rstick": return CGSize(width: 80, height: 90)
        case "select": return CGSize(width: 50, height: 28)
        case "start":  return CGSize(width: 56, height: 28)
        default:       return CGSize(width: 60, height: 40)
        }
    }

    @ViewBuilder
    private var groupContent: some View {
        switch id {
        case "dpad":
            DPadView(size: 100)
        case "action":
            ActionButtonsView(size: 42)
        case "l1":
            PadBtn(label: "L1", w: 100, h: 30, btn: .L1)
        case "l2":
            PadBtn(label: "L2", w: 110, h: 40, btn: .L2)
        case "r1":
            PadBtn(label: "R1", w: 100, h: 30, btn: .R1)
        case "r2":
            PadBtn(label: "R2", w: 110, h: 40, btn: .R2)
        case "lstick":
            StickView(isLeft: true)
        case "rstick":
            StickView(isLeft: false)
        case "select":
            PadBtn(label: "SEL", w: 42, h: 22, btn: .select)
        case "start":
            PadBtn(label: "START", w: 48, h: 22, btn: .start)
        default:
            EmptyView()
        }
    }
}

// MARK: - Clamp helper
private extension CGFloat {
    func clamped(_ lo: CGFloat, _ hi: CGFloat) -> CGFloat {
        Swift.min(Swift.max(self, lo), hi)
    }
}
