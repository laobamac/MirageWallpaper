//
//  EditorContentService.cs
//  Mirage Wallpaper
//
//  Created on 2026/10/28.
//  Copyright © 2026 王孝慈. All rights reserved.
//

using System.Security.Cryptography;
using SteamKit2;
using SteamKit2.CDN;

namespace MirageSteamService;

internal sealed record EditorCompatibility(string Version, string Branch, Dictionary<string, string> SourceHashes);

internal sealed class EditorContentService(SteamSession session, ProtocolWriter writer)
{
    private sealed record Depot(uint Id, ulong ManifestId, byte[] Key, DepotManifest Manifest);
    private sealed record Entry(Depot Depot, DepotManifest.FileData File, string InstallPath);
    private sealed record Plan(string Id, string SteamId, long Epoch, DateTime Created,
        EditorCompatibility Compatibility, Entry[] Files, long DownloadBytes, long InstalledBytes);
    private Plan? prepared;

    internal static string InstallPath(string name)
    {
        var normalized = name.Replace('\\', '/');
        return normalized.StartsWith("distribution/", StringComparison.OrdinalIgnoreCase)
            ? normalized["distribution/".Length..] : normalized;
    }

    internal static bool IsFileValid(Stream stream, ulong expectedSize, byte[] expectedHash)
    {
        if (checked((ulong)stream.Length) != expectedSize) return false;
        // Empty depot files may have an unset SHA-1. Their length fully determines their contents.
        if (expectedSize == 0) return true;
        stream.Position = 0;
        return SHA1.HashData(stream).SequenceEqual(expectedHash);
    }

    public async Task<object> PrepareAsync(EditorCompatibility compatibility, CancellationToken token)
    {
        prepared = null;
        if (compatibility.Branch != "public" || compatibility.SourceHashes.Count < 2 ||
            !compatibility.SourceHashes.ContainsKey("bin/wallpaperui.exe") ||
            !compatibility.SourceHashes.ContainsKey("ui/dist/scripts/scripts.js") ||
            compatibility.SourceHashes.Any(pair => pair.Value.Length != 64 || !pair.Value.All(Uri.IsHexDigit)))
            throw new ServiceException("INVALID_EDITOR_CATALOG", "The editor compatibility catalog is invalid.");
        var steamId = await session.RequireEditorOwnershipAsync(token).ConfigureAwait(false);
        var epoch = session.LicenseEpoch;
        var app = await session.GetAppInfoAsync(token).ConfigureAwait(false);
        var files = new Dictionary<string, Entry>(StringComparer.OrdinalIgnoreCase);
        using var cdn = new Client(session.Client);
        using var cancellation = token.Register(cdn.Dispose);
        foreach (var info in app.KeyValues["depots"].Children)
        {
            if (!uint.TryParse(info.Name, out var id)) continue;
            if (info["config"]["realm"].Value == "steamchina") continue;
            var os = info["config"]["oslist"].Value;
            if (!string.IsNullOrEmpty(os) && !os.Split(',').Contains("windows")) continue;
            if (info["dlcappid"].AsUnsignedInteger() != 0 || info["depotfromapp"].AsUnsignedInteger() != 0) continue;
            var manifestId = info["manifests"][compatibility.Branch]["gid"].AsUnsignedLong();
            if (manifestId == 0) continue;
            var key = await session.Apps.GetDepotDecryptionKey(id, SteamSession.AppId).ToTask().WaitAsync(token).ConfigureAwait(false);
            if (key.Result != EResult.OK) throw new ServiceException("CONTENT_ACCESS_DENIED", "Steam denied access to Wallpaper Engine files.");
            var code = await session.Content.GetManifestRequestCode(id, SteamSession.AppId, manifestId, compatibility.Branch).WaitAsync(token).ConfigureAwait(false);
            if (code == 0) throw new ServiceException("MANIFEST_ACCESS_DENIED", "Steam denied access to this Wallpaper Engine version.");
            DepotManifest? manifest = null;
            for (var attempt = 0; attempt < 4; attempt++)
            {
                var server = await session.GetDownloadServerAsync(id, attempt, token).ConfigureAwait(false);
                try
                {
                    manifest = await cdn.DownloadManifestAsync(id, manifestId, code, server.Server, key.DepotKey, cdnAuthToken: server.Token).WaitAsync(token).ConfigureAwait(false);
                    break;
                }
                catch (Exception) when (!token.IsCancellationRequested && attempt < 3)
                {
                    session.ReportDownloadServerFailure(id, server.Server.Host);
                    await Task.Delay(500 * (attempt + 1), token).ConfigureAwait(false);
                }
            }
            if (manifest?.Files == null) throw new ServiceException("EMPTY_MANIFEST", "Steam returned no Wallpaper Engine files.");
            var depot = new Depot(id, manifestId, key.DepotKey, manifest);
            foreach (var file in manifest.Files)
            {
                if (file.Flags.HasFlag(EDepotFileFlag.Symlink)) throw new ServiceException("UNSAFE_PATH", "A Steam file is a symbolic link.");
                if (file.Flags.HasFlag(EDepotFileFlag.Directory)) continue;
                var name = InstallPath(file.FileName);
                if (!files.ContainsKey(name) || file.FileName.Replace('\\', '/').StartsWith("distribution/", StringComparison.OrdinalIgnoreCase))
                    files[name] = new Entry(depot, file, name);
            }
        }
        if (compatibility.SourceHashes.Keys.Any(path => !files.ContainsKey(path)))
        {
            var related = files.Keys.Where(path => path.Contains("wallpaperui", StringComparison.OrdinalIgnoreCase) || path.Contains("scripts", StringComparison.OrdinalIgnoreCase)).Take(15);
            Console.Error.WriteLine($"Editor manifest layout: {files.Count} files; candidates: {string.Join(", ", related)}; depot keys: {string.Join(", ", app.KeyValues["depots"].Children.Select(item => item.Name))}");
            throw new ServiceException("EDITOR_LAYOUT_UNSUPPORTED", "Steam did not return the expected editor file paths.");
        }
        session.EnsureEditorAccount(steamId, epoch);
        var entries = files.OrderBy(pair => compatibility.SourceHashes.ContainsKey(pair.Key) ? 0 : 1)
            .ThenBy(pair => pair.Key, StringComparer.Ordinal).Select(pair => pair.Value).ToArray();
        var plan = new Plan(Guid.NewGuid().ToString("N"), steamId, epoch, DateTime.UtcNow, compatibility, entries,
            entries.Sum(item => item.File.Chunks.Sum(chunk => (long)chunk.CompressedLength)),
            entries.Sum(item => checked((long)item.File.TotalSize)));
        prepared = plan;
        return new { planId = plan.Id, steamId, version = compatibility.Version, plan.DownloadBytes, plan.InstalledBytes,
            depots = entries.Select(item => new { depotId = item.Depot.Id, manifestId = item.Depot.ManifestId.ToString() }).Distinct().ToArray() };
    }

    public async Task<WorkshopDownloader.DownloadResult> DownloadAsync(string taskId, string planId, string outputRoot, CancellationToken token)
    {
        var plan = prepared;
        if (plan == null || plan.Id != planId || DateTime.UtcNow - plan.Created > TimeSpan.FromMinutes(30))
            throw new ServiceException("EDITOR_PLAN_EXPIRED", "Please check the editor installation size again.");
        session.EnsureEditorAccount(plan.SteamId, plan.Epoch);
        var root = Path.GetFullPath(outputRoot);
        Directory.CreateDirectory(root);
        WorkshopDownloader.ValidateNoLinks(root, token);
        var targets = plan.Files.Select(item => (Entry: item, Path: WorkshopDownloader.ResolveContainedPath(root, item.InstallPath))).ToArray();
        using var cdn = new Client(session.Client);
        using var cancellation = token.Register(cdn.Dispose);
        using var progress = new DownloadProgressTracker(writer, taskId);
        var work = new List<(Entry Entry, string Path, DepotManifest.ChunkData[] Chunks)>();
        long reusable = 0;
        writer.DownloadState(taskId, "validating", totalBytes: plan.DownloadBytes);
        foreach (var target in targets)
        {
            token.ThrowIfCancellationRequested();
            Directory.CreateDirectory(Path.GetDirectoryName(target.Path)!);
            using var stream = new FileStream(target.Path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.Read);
            stream.SetLength(checked((long)target.Entry.File.TotalSize));
            var missing = new List<DepotManifest.ChunkData>();
            foreach (var chunk in target.Entry.File.Chunks)
            {
                token.ThrowIfCancellationRequested();
                if (WorkshopDownloader.IsChunkValid(stream, chunk)) reusable += chunk.CompressedLength;
                else missing.Add(chunk);
            }
            work.Add((target.Entry, target.Path, missing.ToArray()));
        }
        progress.Start(plan.DownloadBytes, reusable);
        using var chunkSlots = new SemaphoreSlim(8, 8);
        async Task DownloadFileAsync((Entry Entry, string Path, DepotManifest.ChunkData[] Chunks) file, CancellationToken fileToken)
        {
            fileToken.ThrowIfCancellationRequested();
            session.EnsureEditorAccount(plan.SteamId, plan.Epoch);
            await Parallel.ForEachAsync(file.Chunks, new ParallelOptions { MaxDegreeOfParallelism = 6, CancellationToken = fileToken }, async (chunk, ct) =>
            {
                await chunkSlots.WaitAsync(ct).ConfigureAwait(false);
                try
                {
                    progress.CurrentFile = file.Entry.InstallPath;
                    for (var attempt = 0; ; attempt++)
                    {
                        ct.ThrowIfCancellationRequested();
                        session.EnsureEditorAccount(plan.SteamId, plan.Epoch);
                        var server = await session.GetDownloadServerAsync(file.Entry.Depot.Id, attempt, ct).ConfigureAwait(false);
                        using var transfer = progress.BeginTransfer(chunk.CompressedLength);
                        DownloadContext.Current.Value = transfer;
                        try
                        {
                            var buffer = new byte[chunk.UncompressedLength];
                            var count = await cdn.DownloadDepotChunkAsync(file.Entry.Depot.Id, chunk, server.Server, buffer, file.Entry.Depot.Key, cdnAuthToken: server.Token).WaitAsync(ct).ConfigureAwait(false);
                            if (count != buffer.Length || DepotChunk.AdlerHash(buffer) != chunk.Checksum)
                                throw new IOException("Steam chunk validation failed.");
                            using var handle = File.OpenHandle(file.Path, FileMode.Open, FileAccess.Write, FileShare.ReadWrite, FileOptions.Asynchronous | FileOptions.RandomAccess);
                            await RandomAccess.WriteAsync(handle, buffer, checked((long)chunk.Offset), ct).ConfigureAwait(false);
                            transfer.Complete();
                            session.ReportDownloadServerSuccess(server.Server.Host);
                            break;
                        }
                        catch (Exception) when (!ct.IsCancellationRequested && attempt < 3)
                        {
                            session.ReportDownloadServerFailure(file.Entry.Depot.Id, server.Server.Host);
                            await Task.Delay(500 * (attempt + 1), ct).ConfigureAwait(false);
                        }
                        finally { DownloadContext.Current.Value = null; }
                    }
                }
                finally { chunkSlots.Release(); }
            }).ConfigureAwait(false);
            using var validation = File.OpenRead(file.Path);
            if (!IsFileValid(validation, file.Entry.File.TotalSize, file.Entry.File.FileHash))
                throw new ServiceException("VALIDATION_FAILED", "Downloaded file failed verification: " + file.Entry.InstallPath);
            var name = file.Entry.InstallPath;
            if (plan.Compatibility.SourceHashes.TryGetValue(name, out var expected))
            {
                validation.Position = 0;
                if (!Convert.ToHexString(SHA256.HashData(validation)).Equals(expected, StringComparison.OrdinalIgnoreCase))
                    throw new ServiceException("EDITOR_VERSION_UNAVAILABLE", "The version provided by Steam needs a newer Mirage editor adapter.");
            }
        }
        foreach (var file in work.Where(item => plan.Compatibility.SourceHashes.ContainsKey(item.Entry.InstallPath)))
            await DownloadFileAsync(file, token).ConfigureAwait(false);
        await Parallel.ForEachAsync(work.Where(item => !plan.Compatibility.SourceHashes.ContainsKey(item.Entry.InstallPath)),
            new ParallelOptions { MaxDegreeOfParallelism = 8, CancellationToken = token },
            async (file, ct) => await DownloadFileAsync(file, ct).ConfigureAwait(false)).ConfigureAwait(false);
        progress.Stop();
        session.EnsureEditorAccount(plan.SteamId, plan.Epoch);
        WorkshopDownloader.ValidateNoLinks(root, token);
        var allowed = targets.Select(item => item.Path).ToHashSet(StringComparer.Ordinal);
        foreach (var path in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
        {
            token.ThrowIfCancellationRequested();
            if (!allowed.Contains(path)) File.Delete(path);
        }
        return new WorkshopDownloader.DownloadResult(plan.DownloadBytes, root);
    }
}
