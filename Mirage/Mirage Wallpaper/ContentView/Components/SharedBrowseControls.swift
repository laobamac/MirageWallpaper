import SwiftUI
import AppKit

struct PageNavigator: View {
    let currentPage: Int
    let pageCount: Int
    let onSelect: (Int) -> Void

    @State private var pageText: String
    @FocusState private var isPageFieldFocused: Bool

    init(currentPage: Int, pageCount: Int, onSelect: @escaping (Int) -> Void) {
        self.currentPage = currentPage
        self.pageCount = max(1, pageCount)
        self.onSelect = onSelect
        _pageText = State(initialValue: String(currentPage))
    }

    var body: some View {
        HStack(spacing: 6) {
            Button {
                select(currentPage - 1)
            } label: {
                Image(systemName: "chevron.left")
                    .frame(width: 18, height: 18)
            }
            .disabled(currentPage <= 1)
            .help("上一页")

            ForEach(Array(pageItems.enumerated()), id: \.offset) { _, item in
                if let page = item {
                    Button {
                        select(page)
                    } label: {
                        Text("\(page)")
                            .font(.callout.weight(page == currentPage ? .semibold : .regular))
                            .foregroundStyle(page == currentPage ? Color.white : Color.primary)
                            .frame(minWidth: 30, minHeight: 30)
                            .background(
                                page == currentPage ? Color.accentColor : Color.clear,
                                in: RoundedRectangle(cornerRadius: 6, style: .continuous)
                            )
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .help("第 \(page) 页")
                } else {
                    Text("…")
                        .foregroundStyle(.secondary)
                        .frame(width: 20, height: 30)
                }
            }

            Button {
                select(currentPage + 1)
            } label: {
                Image(systemName: "chevron.right")
                    .frame(width: 18, height: 18)
            }
            .disabled(currentPage >= pageCount)
            .help("下一页")

            Divider()
                .frame(height: 20)
                .padding(.horizontal, 2)

            TextField("页码", text: $pageText)
                .textFieldStyle(.roundedBorder)
                .multilineTextAlignment(.center)
                .frame(width: 52)
                .focused($isPageFieldFocused)
                .onSubmit(submitPage)
                .onChange(of: pageText) { _, value in
                    let filtered = value.filter(\.isNumber)
                    if value != filtered {
                        pageText = filtered
                    }
                }

            Text("/ \(pageCount)")
                .font(.caption)
                .foregroundStyle(.secondary)
                .monospacedDigit()
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .mirageGlass(
            in: RoundedRectangle(cornerRadius: 8, style: .continuous),
            fallback: AnyShapeStyle(.regularMaterial)
        )
        .onChange(of: currentPage) { _, value in
            pageText = String(value)
        }
        .onChange(of: pageCount) { _, _ in
            pageText = String(min(max(currentPage, 1), pageCount))
        }
        .onChange(of: isPageFieldFocused) { _, focused in
            if !focused {
                submitPage()
            }
        }
    }

    private var pageItems: [Int?] {
        guard pageCount > 7 else {
            return Array(1...pageCount).map(Optional.some)
        }
        if currentPage <= 4 {
            return [1, 2, 3, 4, 5, nil, pageCount]
        }
        if currentPage >= pageCount - 3 {
            return [1, nil, pageCount - 4, pageCount - 3, pageCount - 2, pageCount - 1, pageCount]
        }
        return [1, nil, currentPage - 1, currentPage, currentPage + 1, nil, pageCount]
    }

    private func submitPage() {
        guard let page = Int(pageText) else {
            pageText = String(currentPage)
            return
        }
        select(page)
    }

    private func select(_ page: Int) {
        let value = min(max(page, 1), pageCount)
        pageText = String(value)
        onSelect(value)
    }
}

struct WallpaperGridViewMenu: View {
    @ObservedObject var viewModel: ContentViewModel
    var showsPageSize = false

    var body: some View {
        Menu("视图") {
            Picker("图标大小", selection: $viewModel.explorerIconSize) {
                Text("小图标").tag(Double(140))
                Text("中图标").tag(Double(170))
                Text("大图标").tag(Double(200))
            }
            .pickerStyle(.inline)

            if showsPageSize {
                Divider()
                Picker("每页壁纸数", selection: $viewModel.wallpapersPerPage) {
                    Text("每页 10 个").tag(10)
                    Text("每页 25 个").tag(25)
                    Text("每页 50 个").tag(50)
                }
                .pickerStyle(.inline)
            }
        }
    }
}

struct HorizontalScrollWheelBridge: NSViewRepresentable {
    let onOffsetChange: (CGFloat) -> Void

    func makeNSView(context: Context) -> HorizontalScrollWheelView {
        let view = HorizontalScrollWheelView()
        view.onOffsetChange = onOffsetChange
        return view
    }

    func updateNSView(_ nsView: HorizontalScrollWheelView, context: Context) {
        nsView.onOffsetChange = onOffsetChange
    }
}

final class HorizontalScrollWheelView: NSView {
    var onOffsetChange: ((CGFloat) -> Void)?
    private var monitor: Any?

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(
            rect: bounds,
            options: [.activeInKeyWindow, .mouseEnteredAndExited, .inVisibleRect],
            owner: self
        ))
    }

    override func mouseEntered(with event: NSEvent) {
        installMonitor()
    }

    override func mouseExited(with event: NSEvent) {
        removeMonitor()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil {
            removeMonitor()
        }
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
        nil
    }

    deinit {
        removeMonitor()
    }

    private func installMonitor() {
        guard monitor == nil else { return }
        monitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { [weak self] event in
            guard let self,
                  event.window === self.window,
                  let scrollView = self.enclosingScrollView else { return event }
            let point = self.convert(event.locationInWindow, from: nil)
            guard self.bounds.contains(point) else { return event }
            if abs(event.scrollingDeltaY) >= abs(event.scrollingDeltaX) {
                var ancestor = scrollView.superview
                while let view = ancestor {
                    if let outerScrollView = view as? NSScrollView {
                        outerScrollView.scrollWheel(with: event)
                        return nil
                    }
                    ancestor = view.superview
                }
                return event
            }
            let delta = event.scrollingDeltaX
            guard delta != 0 else { return event }
            let clipView = scrollView.contentView
            let maximumX = max(0, (scrollView.documentView?.bounds.width ?? 0) - clipView.bounds.width)
            var origin = clipView.bounds.origin
            origin.x = min(max(origin.x - delta, 0), maximumX)
            guard abs(origin.x - clipView.bounds.origin.x) > 0.5 else { return event }
            clipView.setBoundsOrigin(origin)
            scrollView.reflectScrolledClipView(clipView)
            self.onOffsetChange?(origin.x)
            return nil
        }
    }

    private func removeMonitor() {
        if let monitor {
            NSEvent.removeMonitor(monitor)
            self.monitor = nil
        }
    }
}
