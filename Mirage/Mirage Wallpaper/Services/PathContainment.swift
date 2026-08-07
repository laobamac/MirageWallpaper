//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

// Containment for relative paths that originate from untrusted third-party
// `project.json` manifests (Steam Workshop packages).
//
// `URL.appending(path:)` does not collapse `..`, so a manifest field such as
// `"file": "../../../../etc/passwd"` produces a real path outside the wallpaper
// directory. Such a path would then reach renderer argv, AVAsset, ImageIO and
// the screen-saver configuration, so every manifest-supplied relative path has
// to be checked against the directory it is supposed to live in.
enum PathContainment {

    // Returns the URL of `relativePath` inside `root`, or nil when it escapes.
    //
    // The verdict is taken on the standardized *and* symlink-resolved pair,
    // which is what makes both `..` segments and symlinks planted inside the
    // downloaded package ineffective.
    //
    // The URL handed back is that same symlink-resolved path, so the file that
    // was validated is the file that gets opened. Returning the unresolved path
    // instead would have it resolved a second time at open() time, and the
    // wallpaper directory is writable by anything running as the user: a
    // component swapped for a symlink in between would escape containment.
    // Use this for every URL that is about to be opened — AVAsset, ImageIO,
    // renderer argv.
    static func containedURL(_ relativePath: String, in root: URL) -> URL? {
        contained(relativePath, in: root)?.resolved
    }

    // Same verdict, but returns the path in the unresolved shape of `root`.
    //
    // Only for the screen-saver configuration file: it stores the unresolved
    // `renderDirectory` next to the entry path, and the screen saver derives
    // the entry's relative path by stripping that prefix from it, so the two
    // have to share one shape. Resolving symlinks there would buy nothing
    // anyway — that file is written now and opened by another process much
    // later, so its window is unbounded by construction. Also used for paths
    // shown to the user, which should look the way the user's disk looks.
    static func containedURLPreservingShape(_ relativePath: String, in root: URL) -> URL? {
        contained(relativePath, in: root)?.unresolved
    }

    // Convenience for callers that only need the verdict.
    static func isContained(_ relativePath: String, in root: URL) -> Bool {
        contained(relativePath, in: root) != nil
    }

    // One containment check, both shapes of the result.
    private static func contained(_ relativePath: String,
                                  in root: URL) -> (resolved: URL, unresolved: URL)? {
        // An empty path denotes the directory itself; nothing can escape.
        guard !relativePath.isEmpty else {
            return (root.standardizedFileURL.resolvingSymlinksInPath(), root)
        }
        let normalizedRoot = root.standardizedFileURL.resolvingSymlinksInPath()
        let candidate = normalizedRoot.appending(path: relativePath)
            .standardizedFileURL.resolvingSymlinksInPath()
        // The trailing "/" matters: without it "/a/bc" would count as inside "/a/b".
        let isInside = candidate.path == normalizedRoot.path
            || candidate.path.hasPrefix(normalizedRoot.path + "/")
        guard isInside else { return nil }
        return (candidate, root.appending(path: relativePath).standardizedFileURL)
    }
}
