//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

// A modern replacement for the plain system `Slider`: a rounded track with an
// accent-tinted progress fill and a soft-shadowed thumb that reacts to hover
// and drag. It mirrors the subset of the `Slider(value:in:step:)` API that
// Mirage actually uses, so it drops straight into existing call sites.
struct MirageSlider<V: BinaryFloatingPoint>: View {
    @Binding var value: V
    let range: ClosedRange<V>
    let step: V?

    init(value: Binding<V>,
         in range: ClosedRange<V>,
         step: V? = nil) {
        self._value = value
        self.range = range.lowerBound < range.upperBound ? range : range.lowerBound...(range.lowerBound + 1)
        self.step = step
    }

    @State private var hovering = false
    @State private var dragging = false

    private let trackHeight: CGFloat = 5
    private let thumbSize: CGFloat = 16

    private var fraction: Double {
        let span = Double(range.upperBound - range.lowerBound)
        guard span > 0 else { return 0 }
        return min(max(Double(value - range.lowerBound) / span, 0), 1)
    }

    var body: some View {
        GeometryReader { geo in
            let usable = max(geo.size.width - thumbSize, 1)
            let x = usable * CGFloat(fraction)

            ZStack(alignment: .leading) {
                Capsule()
                    .fill(Color.primary.opacity(0.14))
                    .frame(height: trackHeight)

                Capsule()
                    .fill(Color.accentColor.opacity(dragging || hovering ? 1.0 : 0.85))
                    .frame(width: x + thumbSize / 2, height: trackHeight)

                Circle()
                    .fill(Color.white)
                    .overlay(
                        Circle().strokeBorder(Color.black.opacity(0.08), lineWidth: 0.5)
                    )
                    .shadow(color: .black.opacity(dragging ? 0.28 : 0.18),
                            radius: dragging ? 4 : 2, y: 1)
                    .frame(width: thumbSize, height: thumbSize)
                    .scaleEffect(dragging ? 1.12 : (hovering ? 1.06 : 1.0))
                    .offset(x: x)
            }
            .frame(height: thumbSize)
            .frame(maxHeight: .infinity)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { g in
                        dragging = true
                        update(fromX: g.location.x - thumbSize / 2, usable: usable)
                    }
                    .onEnded { _ in dragging = false }
            )
            .onHover { hovering = $0 }
            .animation(.easeOut(duration: 0.12), value: hovering)
            .animation(.easeOut(duration: 0.12), value: dragging)
        }
        .frame(height: thumbSize)
    }

    private func update(fromX rawX: CGFloat, usable: CGFloat) {
        let f = min(max(Double(rawX / usable), 0), 1)
        let span = Double(range.upperBound - range.lowerBound)
        var newValue = V(Double(range.lowerBound) + f * span)
        if let step, step > 0 {
            newValue = (newValue / step).rounded() * step
        }
        newValue = min(max(newValue, range.lowerBound), range.upperBound)
        if newValue != value { value = newValue }
    }
}
