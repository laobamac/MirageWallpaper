import SwiftUI

private struct MirageGlassModifier<S: Shape>: ViewModifier {
    let shape: S
    let fallback: AnyShapeStyle
    let interactive: Bool

    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(macOS 26.0, *), interactive {
            content.glassEffect(.regular.interactive(interactive), in: shape)
        } else {
            content.background(fallback, in: shape)
        }
    }
}

extension View {
    func mirageGlass<S: Shape>(in shape: S, fallback: AnyShapeStyle, interactive: Bool = true) -> some View {
        modifier(MirageGlassModifier(shape: shape, fallback: fallback, interactive: interactive))
    }
}
