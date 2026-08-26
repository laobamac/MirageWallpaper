//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AVFoundation
import Darwin
import Foundation
import UniformTypeIdentifiers

final class MirageMemoryVideoAssetLoader: NSObject, AVAssetResourceLoaderDelegate {
    private let data: NSData
    private let assetURL: URL
    private let contentType: String
    private let queue = DispatchQueue(label: "cn.laobamac.Mirage.MemoryVideoAsset")

    init(fileURL: URL) throws {
        let descriptor = open(fileURL.path, O_RDONLY | O_CLOEXEC)
        guard descriptor >= 0 else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
        }

        var status = stat()
        guard fstat(descriptor, &status) == 0, status.st_size > 0,
              UInt64(status.st_size) <= UInt64(Int.max) else {
            let code = errno == 0 ? EINVAL : errno
            close(descriptor)
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(code))
        }

        let length = Int(status.st_size)
        guard let bytes = malloc(length) else {
            close(descriptor)
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(ENOMEM))
        }

        var offset = 0
        while offset < length {
            let count = read(descriptor, bytes.advanced(by: offset), length - offset)
            if count > 0 {
                offset += count
                continue
            }
            if count < 0, errno == EINTR { continue }
            let code = count == 0 ? EIO : errno
            free(bytes)
            close(descriptor)
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(code))
        }
        close(descriptor)

        var components = URLComponents(url: fileURL, resolvingAgainstBaseURL: false)
        components?.scheme = "mirage-memory-video"
        guard let assetURL = components?.url else {
            free(bytes)
            throw URLError(.badURL)
        }

        data = NSData(bytesNoCopy: bytes, length: length, freeWhenDone: true)
        self.assetURL = assetURL
        contentType = UTType(filenameExtension: fileURL.pathExtension)?.identifier ?? UTType.movie.identifier
        super.init()
    }

    func makeAsset() -> AVURLAsset {
        let asset = AVURLAsset(
            url: assetURL,
            options: [AVURLAssetPreferPreciseDurationAndTimingKey: false]
        )
        asset.resourceLoader.setDelegate(self, queue: queue)
        return asset
    }

    func resourceLoader(
        _ resourceLoader: AVAssetResourceLoader,
        shouldWaitForLoadingOfRequestedResource loadingRequest: AVAssetResourceLoadingRequest
    ) -> Bool {
        if let content = loadingRequest.contentInformationRequest {
            let allowed = content.allowedContentTypes
            content.contentType = allowed?.isEmpty != false || allowed?.contains(contentType) == true
                ? contentType : allowed?.first
            content.contentLength = Int64(data.length)
            content.isByteRangeAccessSupported = true
            content.isEntireLengthAvailableOnDemand = true
        }

        guard let request = loadingRequest.dataRequest else {
            loadingRequest.finishLoading()
            return true
        }

        let currentOffset = max(request.currentOffset, request.requestedOffset)
        guard currentOffset >= 0, UInt64(currentOffset) <= UInt64(data.length) else {
            loadingRequest.finishLoading(with: URLError(.badServerResponse))
            return true
        }

        let offset = Int(currentOffset)
        let end: Int
        if request.requestsAllDataToEndOfResource {
            end = data.length
        } else {
            let start = UInt64(max(request.requestedOffset, 0))
            let length = UInt64(max(request.requestedLength, 0))
            let requestedEnd = length > UInt64.max - start ? UInt64.max : start + length
            end = Int(min(requestedEnd, UInt64(data.length)))
        }

        guard offset < end else {
            loadingRequest.finishLoading()
            return true
        }

        let backing = data
        let slice = Data(
            bytesNoCopy: UnsafeMutableRawPointer(mutating: backing.bytes.advanced(by: offset)),
            count: end - offset,
            deallocator: .custom { _, _ in _ = backing }
        )
        request.respond(with: slice)
        loadingRequest.finishLoading()
        return true
    }
}
