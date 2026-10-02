// Copyright © 2026 王孝慈. All rights reserved.
pragma Singleton
import QtQuick

QtObject {
    property int tab: 0
    property int display: 0
    property bool filtersVisible: Boolean(bridge.load("filtersVisible", false))
    property string search: ""
    property int sort: 0
    property bool descending: false
    property int page: 1
    property int pageSize: Number(bridge.load("pageSize", 50))
    property int cardSize: Number(bridge.load("cardSize", 170))
    property string selectedId: ""
    property var favorites: JSON.parse(bridge.load("favorites", "[]"))
    property var runtime: JSON.parse(bridge.load("runtime", "{}"))
    readonly property string displayKey: bridge.screens.length > display ? bridge.screens[display].name : "default"
    property var playlists: JSON.parse(bridge.load("playlists", "{}"))
    readonly property var playlist: (playlists[displayKey] || {}).items || []
    property var savedPlaylists: JSON.parse(bridge.load("savedPlaylists", "[]"))
    readonly property var defaultPlaylistSettings: ({
            order: 0,
            timing: 0,
            hours: 0,
            minutes: 30,
            transition: 0,
            duration: 1,
            anchors: [],
            first: false,
            intro: false,
            videoEnd: false,
            paused: false
        })
    readonly property var playlistSettings: (playlists[displayKey] || {}).settings || defaultPlaylistSettings
    function updatePlaylist(items, settings) {
        let next = Object.assign({}, playlists);
        next[displayKey] = {
            items: items,
            settings: settings || playlistSettings
        };
        playlists = next;
        bridge.save("playlists", JSON.stringify(next));
    }
    property var preferences: Object.assign({}, defaultPreferences, JSON.parse(bridge.load("preferences", "{}")))
    readonly property var defaultPreferences: ({
            appearance: 2,
            language: "system",
            startupSection: 0,
            focus: 0,
            fullscreen: 0,
            audioPlaying: 0,
            sleep: 0,
            battery: 0,
            coverageEnabled: false,
            coverage: 90,
            antialias: 1,
            resolution: 1,
            metalFX: false,
            loadMode: 0,
            animated: 0,
            fps: 30,
            spectrum: true,
            hdr: false,
            autostart: false,
            hideTray: false,
            monochrome: false,
            updates: true,
            beta: false,
            overrideWallpaper: false,
            volume: 1,
            muted: false,
            autoRefresh: true,
            apiKey: "",
            apiEndpoint: 0,
            directDownload: false,
            developer: false
        })
    property var filterGroups: JSON.parse(bridge.load("filters", "{}"))
    readonly property var decoratedLibrary: bridge.library.map(item => Object.assign({}, item, {
            title: (runtime[item.id] || {}).title || item.title,
            tags: (runtime[item.id] || {}).tags || item.tags
        }))
    readonly property var selected: {
        for (const item of decoratedLibrary)
            if (item.id === selectedId)
                return item;
        return ({});
    }
    readonly property var selectedRuntime: runtime[selectedId] || ({
            volume: 1,
            speed: 1,
            fill: 0,
            x: 50,
            y: 50,
            properties: {}
        })
    readonly property var filtered: {
        let result = decoratedLibrary.filter(item => {
            if (search && !String(item.title).toLowerCase().includes(search.toLowerCase()))
                return false;
            const types = ["scene", "video", "web", "application", "preset"];
            if (filterGroups.type && !filterGroups.type.includes(types.indexOf(item.type)))
                return false;
            if (filterGroups.show && filterGroups.show.includes(1) && !favorites.includes(item.id))
                return false;
            if (filterGroups.show && filterGroups.show.includes(0) && !item.approved)
                return false;
            if (filterGroups.show && filterGroups.show.includes(2) && !(item.tags || []).includes("Audio responsive"))
                return false;
            if (filterGroups.show && filterGroups.show.includes(3) && !(item.properties || []).length)
                return false;
            const ratings = ["Everyone", "Questionable", "Mature"];
            if (filterGroups.rating && !filterGroups.rating.includes(Math.max(0, ratings.indexOf(item.contentrating))))
                return false;
            if (filterGroups.source && !filterGroups.source.includes(item.workshopid ? 0 : 1))
                return false;
            if (filterGroups.tags && !filterGroups.tags.some(i => (item.tags || []).some(t => Theme.p(t) === Theme.t(FilterData.tags[i]) || t.toLowerCase() === FilterData.englishTags[i].toLowerCase())))
                return false;
            if (filterGroups.resolution && !filterGroups.resolution.some(label => (item.tags || []).some(t => t.replace(/[^a-z0-9]/gi, "").toLowerCase() === label.replace(/[^a-z0-9]/gi, "").toLowerCase())))
                return false;
            return true;
        });
        result.sort((a, b) => sort === 0 ? String(a.title).localeCompare(String(b.title)) : sort === 1 ? (a.modified || 0) - (b.modified || 0) : String(a.type).localeCompare(String(b.type)));
        if (descending)
            result.reverse();
        return result;
    }
    readonly property int pageCount: Math.max(1, Math.ceil(filtered.length / pageSize))
    readonly property var paged: filtered.slice((Math.min(page, pageCount) - 1) * pageSize, Math.min(page, pageCount) * pageSize)
    property string notice: ""
    property bool rendererConnected: false
    property bool steamConnected: false
    property bool networkBusy: false
    property var workshopItems: []
    property var subscriptions: []
    property var discoverRows: []
    property var downloads: []
    property var bakeTasks: []
    signal request(string action, var payload)
    function changePreference(key, value) {
        let next = Object.assign({}, preferences);
        next[key] = value;
        preferences = next;
        if (key === "language")
            bridge.language = value;
    }
    function commitPreferences() {
        bridge.save("preferences", JSON.stringify(preferences));
        bridge.save("language", preferences.language);
    }
    function setRuntime(key, value) {
        let next = Object.assign({}, runtime);
        let item = Object.assign({}, selectedRuntime);
        item[key] = value;
        next[selectedId] = item;
        runtime = next;
        bridge.save("runtime", JSON.stringify(runtime));
    }
    function setProperty(key, value) {
        let props = Object.assign({}, selectedRuntime.properties);
        props[key] = value;
        setRuntime("properties", props);
    }
    function favorite(id) {
        let next = favorites.slice();
        let i = next.indexOf(id);
        if (i < 0)
            next.push(id);
        else
            next.splice(i, 1);
        favorites = next;
        bridge.save("favorites", JSON.stringify(next));
    }
    function addToPlaylist(id) {
        if (!id || playlist.includes(id))
            return;
        updatePlaylist(playlist.concat([id]));
    }
    function removeFromPlaylist(id) {
        updatePlaylist(playlist.filter(x => x !== id));
    }
    function movePlaylist(from, to) {
        if (from === to || to < 0 || to >= playlist.length)
            return;
        let p = playlist.slice();
        p.splice(to, 0, p.splice(from, 1)[0]);
        updatePlaylist(p);
    }
    function clearPlaylist() {
        updatePlaylist([]);
    }
    function savePlaylist(name) {
        let p = savedPlaylists.filter(x => x.name !== name);
        p.push({
            name: name,
            items: playlist.slice(),
            settings: playlistSettings,
            updated: Date.now()
        });
        savedPlaylists = p;
        bridge.save("savedPlaylists", JSON.stringify(p));
    }
    function find(id) {
        for (const item of decoratedLibrary)
            if (item.id === id)
                return item;
        return {
            id: id,
            title: Theme.t("壁纸不可用"),
            preview: ""
        };
    }
    function apply() {
        if (rendererConnected)
            request("apply", {
                wallpaper: selectedId,
                display: display
            });
        else
            notice = Theme.t("渲染服务尚未连接");
    }
    onSearchChanged: page = 1
    onFilterGroupsChanged: {
        page = 1;
        bridge.save("filters", JSON.stringify(filterGroups));
    }
    onCardSizeChanged: bridge.save("cardSize", cardSize)
    onPageSizeChanged: bridge.save("pageSize", pageSize)
    onFiltersVisibleChanged: bridge.save("filtersVisible", filtersVisible)
    Component.onCompleted: {
        bridge.language = preferences.language;
        tab = preferences.startupSection || 0;
    }
}
