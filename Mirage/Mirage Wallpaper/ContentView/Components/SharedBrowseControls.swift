import SwiftUI
import AppKit

struct PageNavigator: View {
    let currentPage: Int
    let pageCount: Int
    let isLoading: Bool
    let requestedPage: Int?
    let onSelect: (Int) -> Void

    @State private var pageText: String
    @FocusState private var isPageFieldFocused: Bool

    init(currentPage: Int, pageCount: Int, isLoading: Bool = false, requestedPage: Int? = nil,
         onSelect: @escaping (Int) -> Void) {
        self.currentPage = currentPage
        self.pageCount = max(1, pageCount)
        self.isLoading = isLoading
        self.requestedPage = requestedPage
        self.onSelect = onSelect
        _pageText = State(initialValue: String(requestedPage ?? currentPage))
    }

    var body: some View {
        HStack(spacing: 6) {
            if isLoading {
                ProgressView()
                    .controlSize(.small)
                    .help(Text(L("正在加载第 %d 页…", navigationPage)))
                    .accessibilityLabel(Text(L("正在加载第 %d 页…", navigationPage)))
            }

            Button {
                select(navigationPage - 1)
            } label: {
                Image(systemName: "chevron.left")
                    .frame(width: 18, height: 18)
            }
            .disabled(navigationPage <= 1)
            .help("上一页")

            ForEach(Array(pageItems.enumerated()), id: \.offset) { _, item in
                if let page = item {
                    Button {
                        select(page)
                    } label: {
                        Text("\(page)")
                            .font(.callout.weight(page == navigationPage ? .semibold : .regular))
                            .foregroundStyle(page == navigationPage ? Color.white : Color.primary)
                            .frame(minWidth: 30, minHeight: 30)
                            .background(
                                page == navigationPage ? Color.accentColor : Color.clear,
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
                select(navigationPage + 1)
            } label: {
                Image(systemName: "chevron.right")
                    .frame(width: 18, height: 18)
            }
            .disabled(navigationPage >= pageCount)
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
        .onChange(of: navigationPage) { _, value in
            pageText = String(value)
        }
        .onChange(of: isLoading) { _, _ in
            pageText = String(navigationPage)
        }
        .onChange(of: pageCount) { _, _ in
            pageText = String(min(max(navigationPage, 1), pageCount))
        }
        .onChange(of: isPageFieldFocused) { _, focused in
            if !focused {
                submitPage()
            }
        }
    }

    private var navigationPage: Int { requestedPage ?? currentPage }

    private var pageItems: [Int?] {
        guard pageCount > 7 else {
            return Array(1...pageCount).map(Optional.some)
        }
        if navigationPage <= 4 {
            return [1, 2, 3, 4, 5, nil, pageCount]
        }
        if navigationPage >= pageCount - 3 {
            return [1, nil, pageCount - 4, pageCount - 3, pageCount - 2, pageCount - 1, pageCount]
        }
        return [1, nil, navigationPage - 1, navigationPage, navigationPage + 1, nil, pageCount]
    }

    private func submitPage() {
        guard let page = Int(pageText) else {
            pageText = String(navigationPage)
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
    @Bindable var viewModel: ContentViewModel
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

    static func dismantleNSView(_ nsView: HorizontalScrollWheelView, coordinator: Coordinator) {
        nsView.stopObserving()
    }
}

final class HorizontalScrollWheelView: NSView {
    var onOffsetChange: ((CGFloat) -> Void)?
    private var boundsObserver: NSObjectProtocol?
    private weak var observedClipView: NSClipView?

    override func hitTest(_ point: NSPoint) -> NSView? {
        nil
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil {
            stopObserving()
        } else {
            DispatchQueue.main.async { [weak self] in
                self?.startObserving()
            }
        }
    }

    override func viewDidMoveToSuperview() {
        super.viewDidMoveToSuperview()
        startObserving()
    }

    deinit {
        if let boundsObserver {
            NotificationCenter.default.removeObserver(boundsObserver)
        }
    }

    func stopObserving() {
        if let boundsObserver {
            NotificationCenter.default.removeObserver(boundsObserver)
            self.boundsObserver = nil
        }
        observedClipView = nil
    }

    private func startObserving() {
        guard let clipView = enclosingScrollView?.contentView else { return }
        guard observedClipView !== clipView else { return }
        stopObserving()
        clipView.postsBoundsChangedNotifications = true
        observedClipView = clipView
        boundsObserver = NotificationCenter.default.addObserver(
            forName: NSView.boundsDidChangeNotification,
            object: clipView,
            queue: .main
        ) { [weak self] _ in
            guard let self, let clipView = self.observedClipView else { return }
            self.onOffsetChange?(clipView.bounds.origin.x)
        }
    }
}
