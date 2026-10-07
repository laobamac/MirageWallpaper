//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Observation

@Observable
final class WallpaperControlState {
    private(set) var volume: Float = 1
    private(set) var speed: Float = 1
    private(set) var fillMode: FillMode = .cover
    private(set) var position = WallpaperPosition.center

    func update(_ runtime: WallpaperRuntimeState) {
        if volume != runtime.volume { volume = runtime.volume }
        if speed != runtime.speed { speed = runtime.speed }
        if fillMode != runtime.fillMode { fillMode = runtime.fillMode }
        if position != runtime.position { position = runtime.position }
    }
}

@Observable
final class WallpaperPropertyValue {
    fileprivate(set) var value: WEPropertyValue

    init(_ value: WEPropertyValue) { self.value = value }
}

@Observable
final class WallpaperPropertyModel {
    struct Row: Identifiable {
        let id: String
        let property: WEProjectProperty
        let state: WallpaperPropertyValue
    }

    private(set) var rows: [Row] = []
    private(set) var properties: [String: WEProjectProperty] = [:]
    private(set) var overrides: [String: WEPropertyValue] = [:]
    @ObservationIgnored private var values: [String: WallpaperPropertyValue] = [:]

    func update(properties: [String: WEProjectProperty], overrides: [String: WEPropertyValue]) {
        let propertiesChanged = self.properties != properties
        guard propertiesChanged || self.overrides != overrides else { return }
        if propertiesChanged {
            self.properties = properties
            values = values.filter { properties[$0.key] != nil }
            rows = WEProjectProperties(items: properties).sorted.compactMap { key, property in
                guard !property.isPresetOnly else { return nil }
                let state = values[key] ?? WallpaperPropertyValue(overrides[key] ?? property.value)
                values[key] = state
                return Row(id: key, property: property, state: state)
            }
        }
        let changedKeys = propertiesChanged ? Set(values.keys)
            : Set(self.overrides.keys).union(overrides.keys).filter { self.overrides[$0] != overrides[$0] }
        for key in changedKeys {
            guard let state = values[key], let property = properties[key] else { continue }
            let value = overrides[key] ?? property.value
            if state.value != value { state.value = value }
        }
        if self.overrides != overrides { self.overrides = overrides }
    }

    func setOverride(_ value: WEPropertyValue, for key: String) {
        guard let property = properties[key] else { return }
        let normalized = property.normalizedComboValue(value)
        if values[key]?.value != normalized { values[key]?.value = normalized }
        if overrides[key] != normalized { overrides[key] = normalized }
    }
}
