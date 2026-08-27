// MetalGameView.swift — UIViewRepresentable wrapper for CAMetalLayer game view
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

struct MetalGameView: UIViewRepresentable {
    func makeUIView(context: Context) -> UIView {
        return iPSX2Bridge.gameRenderView()
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        // drawableSize update is handled by layoutSubviews
    }
}
