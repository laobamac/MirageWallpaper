//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

final class WallpaperEngineExploreAPI {
    static let shared = WallpaperEngineExploreAPI()

    private let endpoint = URL(string: "https://www.wallpaperengineapi.com/api/explore/v1")!
    private let session: URLSession = {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 15
        configuration.timeoutIntervalForResource = 30
        return URLSession(configuration: configuration)
    }()
    private let decoder = JSONDecoder()
    private var memoryCache: [WEExploreDefinition]?
    private let cacheURL: URL = {
        let directory = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appending(path: "Mirage/Explore", directoryHint: .isDirectory)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory.appending(path: "feed-v1.json")
    }()
    private let etagKey = "WallpaperEngineExploreETagV1"

    func fetch(force: Bool = false) async throws -> [WEExploreDefinition] {
        if !force, let memoryCache { return memoryCache }
        var request = URLRequest(url: endpoint)
        if let etag = UserDefaults.standard.string(forKey: etagKey),
           FileManager.default.fileExists(atPath: cacheURL.path) {
            request.setValue(etag, forHTTPHeaderField: "If-None-Match")
        }
        do {
            let (data, response) = try await session.data(for: request)
            guard let http = response as? HTTPURLResponse else {
                throw WallpaperEngineExploreError.invalidResponse
            }
            if http.statusCode == 304, let cached = try? Data(contentsOf: cacheURL) {
                return try decode(cached)
            }
            guard http.statusCode == 200 else {
                throw WallpaperEngineExploreError.httpError(http.statusCode)
            }
            let items = try decode(data)
            try? data.write(to: cacheURL, options: .atomic)
            if let etag = http.value(forHTTPHeaderField: "ETag") {
                UserDefaults.standard.set(etag, forKey: etagKey)
            }
            return items
        } catch {
            if let cached = try? Data(contentsOf: cacheURL) {
                return try decode(cached)
            }
            throw error
        }
    }

    private func decode(_ data: Data) throws -> [WEExploreDefinition] {
        let items = try decoder.decode(WEExploreResponse.self, from: data).response.items
        memoryCache = items
        return items
    }
}

enum WallpaperEngineExploreError: LocalizedError {
    case invalidResponse
    case httpError(Int)

    var errorDescription: String? {
        switch self {
        case .invalidResponse: return L("Wallpaper Engine 发现页返回了无效响应")
        case .httpError(let code): return L("Wallpaper Engine 发现页请求失败：HTTP %@", String(code))
        }
    }
}
