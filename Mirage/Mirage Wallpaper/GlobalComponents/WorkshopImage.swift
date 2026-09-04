//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import SwiftUI

@MainActor
private final class WorkshopImageStaticState: ObservableObject {
    enum Phase {
        case loading
        case success(URL, NSImage)
        case failure(URL?)
    }

    @Published private(set) var phase: Phase = .loading

    func showLoading() {
        if case .loading = phase { return }
        phase = .loading
    }

    func showLoadingIfFailed(for url: URL) {
        guard case .failure(let currentURL) = phase, currentURL == url else { return }
        phase = .loading
    }

    func show(_ image: NSImage, for url: URL) {
        if case .success(let currentURL, let currentImage) = phase,
           currentURL == url, currentImage === image {
            return
        }
        phase = .success(url, image)
    }

    func showFailure(for url: URL?) {
        if case .success(let currentURL, _) = phase, currentURL == url { return }
        if case .failure(let currentURL) = phase, currentURL == url { return }
        phase = .failure(url)
    }
}

@MainActor
private final class WorkshopImageAnimationState: ObservableObject {
    struct Snapshot {
        let descriptor: WorkshopImageAnimationDescriptor?
        let payload: WorkshopImageAnimation?
    }

    @Published private(set) var snapshot = Snapshot(
        descriptor: nil,
        payload: nil
    )

    func setDescriptor(_ descriptor: WorkshopImageAnimationDescriptor?) {
        if snapshot.descriptor?.identity != descriptor?.identity {
            publish(descriptor: descriptor, payload: nil)
        } else {
            publish(descriptor: descriptor, payload: snapshot.payload)
        }
    }

    func show(
        _ payload: WorkshopImageAnimation,
        for descriptor: WorkshopImageAnimationDescriptor
    ) {
        guard snapshot.descriptor?.identity == descriptor.identity,
              payload.identity == descriptor.identity else { return }
        publish(descriptor: descriptor, payload: payload)
    }

    func hidePayload() {
        publish(descriptor: snapshot.descriptor, payload: nil)
    }

    func fail(_ descriptor: WorkshopImageAnimationDescriptor) {
        guard snapshot.descriptor?.identity == descriptor.identity else { return }
        hidePayload()
    }

    func reset() {
        publish(descriptor: nil, payload: nil)
    }

    private func publish(
        descriptor: WorkshopImageAnimationDescriptor?,
        payload: WorkshopImageAnimation?
    ) {
        guard snapshot.descriptor != descriptor || snapshot.payload != payload else { return }
        snapshot = Snapshot(descriptor: descriptor, payload: payload)
    }
}

@MainActor
private final class WorkshopImageController: ObservableObject {
    struct ImageRequest: Equatable {
        let url: URL?
        let maxPixel: Int
        let isSizeReady: Bool
        let isLoadingEnabled: Bool
    }

    struct AnimationRequest: Equatable {
        let expectedURL: URL?
        let descriptor: WorkshopImageAnimationDescriptor?
        let isAnimating: Bool
        let isLoadingEnabled: Bool
    }

    let staticState = WorkshopImageStaticState()
    let animationState = WorkshopImageAnimationState()

    private let loader: WorkshopImageLoader
    private var sourceURL: URL?

    init(loader: WorkshopImageLoader = WorkshopImageLoader.shared) {
        self.loader = loader
    }

    func loadImage(_ request: ImageRequest) async {
        guard !Task.isCancelled else { return }

        if sourceURL != request.url {
            sourceURL = request.url
            staticState.showLoading()
            animationState.reset()
        }

        guard request.isSizeReady else { return }
        guard request.isLoadingEnabled else {
            animationState.reset()
            return
        }
        guard let url = request.url else {
            staticState.showFailure(for: nil)
            animationState.reset()
            return
        }

        staticState.showLoadingIfFailed(for: url)
        do {
            let loaded = try await loader.load(url: url, maxPixel: request.maxPixel)
            try Task.checkCancellation()
            guard sourceURL == url else { return }
            staticState.show(loaded.image, for: url)
            animationState.setDescriptor(loaded.animationDescriptor)
        } catch is CancellationError {
            return
        } catch {
            guard !Task.isCancelled, sourceURL == url else { return }
            staticState.showFailure(for: url)
            animationState.reset()
        }
    }

    func loadAnimation(_ request: AnimationRequest) async {
        guard !Task.isCancelled else { return }

        guard request.isLoadingEnabled else {
            animationState.reset()
            return
        }
        guard request.isAnimating else {
            animationState.hidePayload()
            return
        }
        guard let expectedURL = request.expectedURL,
              sourceURL == expectedURL,
              let descriptor = request.descriptor,
              descriptor.url == expectedURL,
              animationState.snapshot.descriptor?.identity == descriptor.identity else {
            animationState.reset()
            return
        }
        do {
            let payload = try await loader.loadAnimation(descriptor)
            try Task.checkCancellation()
            guard sourceURL == expectedURL,
                  animationState.snapshot.descriptor?.identity == descriptor.identity else {
                return
            }
            animationState.show(payload, for: descriptor)
        } catch is CancellationError {
            return
        } catch {
            guard !Task.isCancelled else { return }
            animationState.fail(descriptor)
        }
    }

    func disappear() {
        animationState.reset()
    }
}

struct WorkshopImage: View {
    let url: URL?
    var contentMode: ContentMode = .fill
    var isAnimating = false
    var isLoadingEnabled = true

    @Environment(\.displayScale) private var displayScale
    @StateObject private var controller = WorkshopImageController()
    @State private var sizing = WorkshopImageSizing.pending

    private var imageRequest: WorkshopImageController.ImageRequest {
        WorkshopImageController.ImageRequest(
            url: url,
            maxPixel: sizing.maxPixel,
            isSizeReady: sizing.isSizeReady,
            isLoadingEnabled: isLoadingEnabled
        )
    }

    var body: some View {
        Rectangle()
            .fill(Color.secondary.opacity(0.10))
            .overlay {
                ZStack {
                    WorkshopImageStaticLayer(
                        state: controller.staticState,
                        expectedURL: url,
                        contentMode: contentMode
                    )
                    WorkshopImageAnimationLayer(
                        controller: controller,
                        state: controller.animationState,
                        expectedURL: url,
                        contentMode: contentMode,
                        isAnimating: isAnimating,
                        isLoadingEnabled: isLoadingEnabled
                    )
                }
            }
            .clipped()
            .background {
                WorkshopImageSizeReader(sizing: $sizing, scale: displayScale)
            }
            .task(id: imageRequest) {
                await controller.loadImage(imageRequest)
            }
            .onDisappear {
                controller.disappear()
            }
    }
}

private struct WorkshopImageStaticLayer: View {
    @ObservedObject var state: WorkshopImageStaticState
    let expectedURL: URL?
    let contentMode: ContentMode

    var body: some View {
        switch state.phase {
        case .loading:
            ProgressView()
                .controlSize(.small)
        case .success(let url, let image) where url == expectedURL:
            Image(nsImage: image)
                .resizable()
                .interpolation(.high)
                .aspectRatio(contentMode: contentMode)
        case .failure(let url) where url == expectedURL:
            Image(systemName: "photo")
                .font(.title2)
                .foregroundStyle(.tertiary)
        default:
            ProgressView()
                .controlSize(.small)
        }
    }
}

private struct WorkshopImageAnimationLayer: View {
    let controller: WorkshopImageController
    @ObservedObject var state: WorkshopImageAnimationState
    let expectedURL: URL?
    let contentMode: ContentMode
    let isAnimating: Bool
    let isLoadingEnabled: Bool

    private var request: WorkshopImageController.AnimationRequest {
        WorkshopImageController.AnimationRequest(
            expectedURL: expectedURL,
            descriptor: isAnimating && isLoadingEnabled ? state.snapshot.descriptor : nil,
            isAnimating: isAnimating,
            isLoadingEnabled: isLoadingEnabled
        )
    }

    var body: some View {
        Group {
            if isLoadingEnabled, isAnimating,
               state.snapshot.descriptor?.url == expectedURL,
               let payload = state.snapshot.payload {
                WorkshopAnimatedImage(
                    image: payload.image,
                    identity: payload.identity,
                    contentMode: contentMode
                )
            }
        }
        .task(id: request) {
            await controller.loadAnimation(request)
        }
    }
}

private struct WorkshopImageSizing: Equatable {
    static let pending = WorkshopImageSizing(maxPixel: 64, isSizeReady: false)

    let maxPixel: Int
    let isSizeReady: Bool

    init(size: CGSize, scale: CGFloat) {
        maxPixel = WorkshopImageLoader.maxPixel(for: size, scale: scale)
        isSizeReady = size.width > 1 && size.height > 1
    }

    private init(maxPixel: Int, isSizeReady: Bool) {
        self.maxPixel = maxPixel
        self.isSizeReady = isSizeReady
    }
}

private struct WorkshopImageSizeReader: View {
    @Binding var sizing: WorkshopImageSizing
    let scale: CGFloat

    var body: some View {
        GeometryReader { proxy in
            let newSizing = WorkshopImageSizing(size: proxy.size, scale: scale)
            Color.clear
                .task(id: newSizing) {
                    guard !Task.isCancelled, sizing != newSizing else { return }
                    sizing = newSizing
                }
        }
    }
}

private struct WorkshopAnimatedImage: NSViewRepresentable {
    let image: NSImage
    let identity: String
    let contentMode: ContentMode

    final class Coordinator {
        var identity: String?
    }

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeNSView(context: Context) -> WorkshopAnimatedNSView {
        let view = WorkshopAnimatedNSView()
        updateNSView(view, context: context)
        return view
    }

    func updateNSView(_ view: WorkshopAnimatedNSView, context: Context) {
        view.contentMode = contentMode
        if context.coordinator.identity != identity {
            context.coordinator.identity = identity
            view.image = image
        }
    }

    static func dismantleNSView(_ view: WorkshopAnimatedNSView, coordinator: Coordinator) {
        view.image = nil
        coordinator.identity = nil
    }
}

private final class WorkshopAnimatedNSView: NSView {
    private let imageView = NSImageView()
    var contentMode: ContentMode = .fill {
        didSet { needsLayout = true }
    }
    var image: NSImage? {
        get { imageView.image }
        set {
            imageView.image = newValue
            imageView.animates = newValue != nil
            needsLayout = true
        }
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.masksToBounds = true
        imageView.imageScaling = .scaleProportionallyUpOrDown
        imageView.imageAlignment = .alignCenter
        imageView.autoresizingMask = []
        addSubview(imageView)
    }

    required init?(coder: NSCoder) {
        nil
    }

    override func layout() {
        super.layout()
        guard contentMode == .fill, let size = imageView.image?.size,
              size.width > 0, size.height > 0,
              bounds.width > 0, bounds.height > 0 else {
            imageView.frame = bounds
            return
        }
        let scale = max(bounds.width / size.width, bounds.height / size.height)
        let width = size.width * scale
        let height = size.height * scale
        imageView.frame = NSRect(
            x: (bounds.width - width) / 2,
            y: (bounds.height - height) / 2,
            width: width,
            height: height
        )
    }
}
