// Service state comes from MirageController; this singleton owns UI state and
// QSettings-backed preferences so the platform bridge stays at system I/O.
pragma Singleton
import QtQuick
import QtCore
import "ConditionEvaluator.js" as ConditionEvaluator

QtObject {
    // QtObject has no default child list; keep stateful QML objects as explicit properties.
    property Settings uiSettings: Settings {}
    property int tab: Number(Store.load("startupSection", 0))
    property int display: controller.playlistScreen
    property bool filtersVisible: Boolean(Store.load("filtersVisible", false))
    property string search: ""
    property int sort: 0
    property bool descending: false
    property int page: 1
    property int pageSize: Number(Store.load("pageSize", 50))
    property int cardSize: Number(Store.load("cardSize", 170))
    property string selectedId: controller.selectedWallpaperId
    property var preferences: Object.assign({}, defaultPreferences, JSON.parse(Store.load("preferences", "{}")))
    property var filterGroups: JSON.parse(Store.load("filters", "{}"))
    property string notice: ""
    property string pendingApplyId: ""
    property bool pendingApplyAllScreens: false
    property string language: String(uiSettings.value("language", "system"))
    property var sources: uiSettings.value("sources", [])
    property var hiddenWallpapers: uiSettings.value("hiddenWallpapers", [])
    property bool busy: false
    property var conditions: ({})
    signal trustConfirmationRequested()
    readonly property var library: controller.wallpapers.filter(item => !hiddenWallpapers.includes(item.id))
    readonly property bool rendererConnected: controller.displays.length > 0
    readonly property bool steamConnected: controller.steamLoggedIn
    readonly property bool networkBusy: controller.workshopLoading || controller.discoverLoading || controller.subscriptionsLoading
    readonly property var workshopItems: controller.workshopItems
    readonly property var subscriptions: controller.subscriptions
    readonly property var discoverRows: controller.discoverSections
    readonly property var downloads: controller.downloadQueue
    readonly property var bakeTasks: controller.bakeTasks
    readonly property var favorites: library.filter(item => item.favorite).map(item => item.id)
    readonly property var decoratedLibrary: library
    readonly property var selected: {
        for (const item of decoratedLibrary) {
            if (item.id === selectedId) return item;
        }
        return controller.selectedWallpaper;
    }
    readonly property var selectedRuntime: ({
        title: selected.title || "",
        tags: selected.tags || [],
        volume: controller.selectedVolume,
        speed: controller.selectedSpeed,
        fill: ["cover", "contain", "stretch"].indexOf(controller.selectedFillMode),
        x: controller.selectedPositionX * 100,
        y: controller.selectedPositionY * 100,
        properties: controller.selectedProperties.reduce((values, property) => {
            values[property.key] = property.value;
            return values;
        }, ({}))
    })
    readonly property var playlist: controller.playlistItems.map(item => item.id)
    readonly property var savedPlaylists: controller.savedPlaylists
    readonly property var playlistSettings: ({
        order: controller.playlistSettings.order === "random" ? 0 : 1,
        timing: ({ timer: 0, daytime: 1, dayOfWeek: 2, logon: 3, never: 4 })[controller.playlistSettings.timing] || 0,
        hours: controller.playlistSettings.timerHours,
        minutes: controller.playlistSettings.timerMinutes,
        transition: controller.playlistSettings.transition === "disabled" ? 0 : 1,
        duration: controller.playlistSettings.transitionSeconds,
        anchors: controller.playlistSettings.daytimeAnchors,
        first: controller.playlistSettings.alwaysBeginFirst,
        intro: controller.playlistSettings.introOnStartup,
        videoEnd: controller.playlistSettings.videoSequence,
        paused: controller.playlistSettings.updateOnPause
    })
    readonly property var defaultPreferences: ({
        appearance: 2, language: "system", startupSection: 0, focus: 0, fullscreen: 0,
        audioPlaying: 0, sleep: 0, battery: 0, coverageEnabled: false, coverage: 90,
        antialias: 1, resolution: 1, metalFX: false, loadMode: 0, animated: 0,
        fps: 30, spectrum: true, hdr: false, autostart: false, hideTray: false,
        monochrome: false, updates: true, beta: false, overrideWallpaper: false,
        volume: 1, muted: false, autoRefresh: true, apiKey: "", apiEndpoint: 0,
        directDownload: false, developer: false
    })
    readonly property var filtered: {
        let result = decoratedLibrary.filter(item => {
            if (search && !String(item.searchText || item.title).toLowerCase().includes(search.toLowerCase())) return false;
            const types = ["scene", "video", "web", "application", "preset"];
            if (filterGroups.type && !filterGroups.type.includes(types.indexOf(item.type))) return false;
            if (filterGroups.show && filterGroups.show.includes(1) && !item.favorite) return false;
            if (filterGroups.show && filterGroups.show.includes(0) && !item.approved) return false;
            if (filterGroups.show && filterGroups.show.includes(2) && !(item.tags || []).includes("Audio responsive")) return false;
            if (filterGroups.show && filterGroups.show.includes(3) && !(item.properties || []).length) return false;
            const ratings = ["Everyone", "Questionable", "Mature"];
            if (filterGroups.rating && !filterGroups.rating.includes(Math.max(0, ratings.indexOf(item.contentrating)))) return false;
            if (filterGroups.source && !filterGroups.source.includes(item.workshopid ? 0 : 1)) return false;
            if (filterGroups.tags && !filterGroups.tags.some(index => (item.tags || []).some(tag => Theme.p(tag) === Theme.t(FilterData.tags[index]) || tag.toLowerCase() === FilterData.englishTags[index].toLowerCase()))) return false;
            if (filterGroups.resolution && !filterGroups.resolution.some(label => (item.tags || []).some(tag => tag.replace(/[^a-z0-9]/gi, "").toLowerCase() === label.replace(/[^a-z0-9]/gi, "").toLowerCase()))) return false;
            return true;
        });
        result.sort((left, right) => sort === 0 ? String(left.title).localeCompare(String(right.title)) : sort === 1 ? (left.modified || 0) - (right.modified || 0) : String(left.type).localeCompare(String(right.type)));
        if (descending) result.reverse();
        return result;
    }
    readonly property int pageCount: Math.max(1, Math.ceil(filtered.length / pageSize))
    readonly property var paged: filtered.slice((Math.min(page, pageCount) - 1) * pageSize, Math.min(page, pageCount) * pageSize)

    // Loads one value from the established MirageQtUI settings namespace.
    // Signature: load(key: string, fallback: var) -> var; stored values keep their original type.
    function load(key, fallback) { return uiSettings.value(key, fallback); }
    // Persists one UI preference without changing the existing QSettings key format.
    // Signature: save(key: string, value: var) -> void.
    function save(key, value) { uiSettings.setValue(key, value); }
    // Adds a local folder and updates the service-owned library source list.
    // Signature: addSource(folderUrl: url) -> void; FolderDialog supplies a local file URL.
    function addSource(folderUrl) {
        const path = folderUrl.toLocalFile();
        if (sources.includes(path)) return;
        const next = sources.slice();
        next.push(path);
        sources = next;
        save("sources", next);
        controller.setLibrarySources(next);
        refresh();
    }
    // Removes a configured path and reloads the service-backed catalog.
    // Signature: removeSource(path: string) -> void.
    function removeSource(path) {
        const next = sources.filter(source => source !== path);
        if (next.length === sources.length) return;
        sources = next;
        save("sources", next);
        controller.setLibrarySources(next);
        refresh();
    }
    // Hides a wallpaper in this UI without deleting service-owned files.
    // Signature: hideWallpaper(id: string) -> void.
    function hideWallpaper(id) {
        if (hiddenWallpapers.includes(id)) return;
        hiddenWallpapers = hiddenWallpapers.concat([id]);
        save("hiddenWallpapers", hiddenWallpapers);
    }
    // Coalesces repeated catalog refresh requests onto one GUI event-loop turn.
    // Signature: refresh() -> void.
    function refresh() {
        if (busy) return;
        busy = true;
        refreshTimer.start();
    }
    property Timer refreshTimer: Timer {
        interval: 0
        onTriggered: {
            controller.reloadWallpapers();
            Store.busy = false;
        }
    }
    // Evaluates wallpaper-property conditions in QML's existing JS runtime.
    // Signature: evaluateConditions(properties: object[], overrides: object) -> void.
    function evaluateConditions(properties, overrides) {
        conditions = ConditionEvaluator.evaluate(properties, overrides);
    }
    // Updates one preference while retaining its established JSON storage format.
    // Signature: changePreference(key: string, value: var) -> void.
    function changePreference(key, value) {
        const next = Object.assign({}, preferences);
        next[key] = value;
        preferences = next;
        if (key === "language") language = value;
        if (key === "startupSection") Store.save("startupSection", value);
    }
    // Applies the UI preference snapshot through MirageController's service API.
    // Signature: commitPreferences() -> void.
    function commitPreferences() {
        Store.save("preferences", JSON.stringify(preferences));
        language = preferences.language;
        const antiAliasing = ["none", "msaa_x2", "msaa_x4", "msaa_x8"][preferences.antialias];
        const textureResolution = ["highQuality", "automatic", "highPerformance"][preferences.resolution];
        const focusRules = ["keepRunning", "mute", "pause"];
        const fullscreenRules = ["keepRunning", "mute", "pause", "stop"];
        const pauseStopRules = ["keepRunning", "pause", "stop"];
        const settings = {
            fps: preferences.fps,
            antiAliasing: antiAliasing,
            textureResolution: textureResolution,
            wallpaperLoadSource: preferences.loadMode === 0 ? "disk" : "memory",
            enableSpectrum: preferences.spectrum,
            autoStart: preferences.autostart,
            masterVolume: preferences.volume,
            globalMuted: preferences.muted,
            autoRefresh: preferences.autoRefresh,
            steamAPIKey: preferences.apiKey,
            steamAPIEndpoint: preferences.apiEndpoint === 0 ? "official" : "mirror",
            otherApplicationFocused: focusRules[preferences.focus],
            otherApplicationFullscreen: fullscreenRules[preferences.fullscreen],
            otherApplicationPlayingAudio: focusRules[preferences.audioPlaying],
            displayAsleep: pauseStopRules[preferences.sleep],
            laptopOnBattery: pauseStopRules[preferences.battery],
            appearance: ["light", "dark", "followSystem"][preferences.appearance]
        };
        if (!controller.applySettings(settings)) notice = controller.statusMessage;
    }
    // Maps editor values to the existing wallpaper runtime API.
    // Signature: setRuntime(key: string, value: var) -> void.
    function setRuntime(key, value) {
        if (key === "title" || key === "tags") {
            const title = key === "title" ? value : selected.title;
            const tags = key === "tags" ? value : selected.tags;
            controller.updateSelectedMetadata(title, tags);
        } else if (key === "volume") {
            controller.selectedVolume = value;
        } else if (key === "speed") {
            controller.selectedSpeed = value;
        } else if (key === "fill") {
            controller.selectedFillMode = ["cover", "contain", "stretch"][value];
        } else if (key === "x") {
            controller.selectedPositionX = value / 100;
        } else if (key === "y") {
            controller.selectedPositionY = value / 100;
        } else if (key === "properties") {
            // Imported presets replace the saved overrides before applying values.
            controller.resetSelectedProperties();
            for (const property in value)
                controller.setSelectedProperty(property, value[property]);
        }
    }
    // Persists one custom wallpaper property through the runtime service.
    // Signature: setProperty(key: string, value: var) -> void.
    function setProperty(key, value) { controller.setSelectedProperty(key, value); }
    // Toggles the service-owned favorite state for a wallpaper ID.
    // Signature: favorite(id: string) -> void.
    function favorite(id) { controller.toggleFavorite(id); }
    // Adds a wallpaper to the active playlist.
    // Signature: addToPlaylist(id: string) -> void.
    function addToPlaylist(id) {
        controller.addWallpaperToPlaylist(id);
    }
    // Removes one item from the active playlist.
    // Signature: removeFromPlaylist(id: string) -> void.
    function removeFromPlaylist(id) { controller.removePlaylistItem(id); }
    // Reorders the active playlist by source and destination indices.
    // Signature: movePlaylist(from: number, to: number) -> void.
    function movePlaylist(from, to) { controller.movePlaylistItem(from, to); }
    // Clears the active playlist through PlaylistManager.
    // Signature: clearPlaylist() -> void.
    function clearPlaylist() { controller.clearPlaylist(); }
    // Saves the active playlist under a user-provided name.
    // Signature: savePlaylist(name: string) -> void.
    function savePlaylist(name) { controller.savePlaylist(name); }
    // Converts UI playlist settings to the service contract.
    // Signature: updatePlaylist(items: object[], settings: object) -> void.
    function updatePlaylist(items, settings) {
        const mapped = {
            order: settings.order === 0 ? "random" : "sorted",
            timing: ["timer", "daytime", "dayOfWeek", "logon", "never"][settings.timing],
            timerHours: settings.hours,
            timerMinutes: settings.minutes,
            transition: settings.transition === 0 ? "disabled" : "enabled",
            transitionSeconds: settings.duration,
            daytimeAnchors: settings.anchors,
            alwaysBeginFirst: settings.first,
            introOnStartup: settings.intro,
            videoSequence: settings.videoEnd,
            updateOnPause: settings.paused
        };
        controller.updatePlaylistSettings(mapped);
    }
    // Finds a visible wallpaper or returns the established unavailable row.
    // Signature: find(id: string) -> object.
    function find(id) {
        for (const item of decoratedLibrary) {
            if (item.id === id) return item;
        }
        return ({ id: id, title: Theme.t("壁纸不可用"), preview: "" });
    }
    // Applies the selected wallpaper after honoring the renderer trust gate.
    // Signature: apply(allScreens: boolean) -> void.
    function apply(allScreens) {
        if (rendererConnected) {
            // Match the renderer trust gate with a UI confirmation before playback starts.
            if (selected.type === "web" && !controller.isWallpaperTrusted(selectedId)) {
                pendingApplyId = selectedId;
                pendingApplyAllScreens = Boolean(allScreens);
                trustConfirmationRequested();
                return;
            }
            applyWallpaper(selectedId, Boolean(allScreens));
        } else {
            notice = Theme.t("渲染服务尚未连接");
        }
    }
    // Applies a trusted wallpaper to the selected screen or every screen.
    // Signature: applyWallpaper(id: string, allScreens: boolean) -> void.
    function applyWallpaper(id, allScreens) {
        controller.selectWallpaper(id);
        if (allScreens)
            controller.applySelected(true);
        else
            controller.applySelectedToScreen(display);
    }
    // Completes a pending trust confirmation and starts playback.
    // Signature: confirmPendingApply(persist: boolean) -> void.
    function confirmPendingApply(persist) {
        const id = pendingApplyId;
        const allScreens = pendingApplyAllScreens;
        pendingApplyId = "";
        pendingApplyAllScreens = false;
        if (id === "") return;
        controller.trustWallpaper(id, persist);
        applyWallpaper(id, allScreens);
    }
    // Drops an unconfirmed wallpaper apply request.
    // Signature: cancelPendingApply() -> void.
    function cancelPendingApply() {
        pendingApplyId = "";
        pendingApplyAllScreens = false;
    }

    onSelectedIdChanged: if (controller.selectedWallpaperId !== selectedId) controller.selectWallpaper(selectedId)
    onDisplayChanged: controller.playlistScreen = display
    onSearchChanged: page = 1
    onFilterGroupsChanged: {
        page = 1;
        Store.save("filters", JSON.stringify(filterGroups));
    }
    onCardSizeChanged: Store.save("cardSize", cardSize)
    onPageSizeChanged: Store.save("pageSize", pageSize)
    onFiltersVisibleChanged: Store.save("filtersVisible", filtersVisible)
    onPreferencesChanged: {
        if (preferences.language !== language) language = preferences.language;
    }
    // Signature: onLanguageChanged() -> void; reloads QML-owned translation tables for the selected locale.
    onLanguageChanged: {
        uiSettings.setValue("language", language);
        Theme.loadTranslations(language);
    }
    Component.onCompleted: {
        language = preferences.language;
        controller.setLibrarySources(sources);
        controller.loadDiscover();
    }
    property Connections backendConnection: Connections {
        target: controller
        // Signature: onSelectedWallpaperChanged() -> void; mirrors service selection into transient UI selection.
        function onSelectedWallpaperChanged() {
            if (Store.selectedId !== controller.selectedWallpaperId) Store.selectedId = controller.selectedWallpaperId;
        }
        // Signature: onPlaylistChanged() -> void; keeps the selected display aligned with the service.
        function onPlaylistChanged() {
            if (Store.display !== controller.playlistScreen) Store.display = controller.playlistScreen;
        }
    }
}
