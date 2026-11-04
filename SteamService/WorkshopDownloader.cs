//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

using System.Collections.Concurrent;
using System.Security.Cryptography;
using SteamKit2;
using SteamKit2.CDN;
using SteamKit2.Internal;

namespace MirageSteamService;

internal sealed class WorkshopDownloader
{
    internal sealed record DownloadResult(long TotalBytes, string OutputPath);

    private static readonly SemaphoreSlim GlobalChunkSlots = new(8, 8);
    private static readonly ConcurrentDictionary<string, byte> ActiveInstallations = new(WorkshopStorage.PathComparer);
    private readonly SteamSession session;
    private readonly ProtocolWriter writer;

    public WorkshopDownloader(SteamSession session, ProtocolWriter writer)
    {
        this.session = session;
        this.writer = writer;
    }

    public async Task<DownloadResult> DownloadAsync(string taskId, ulong workshopId, string outputRoot, CancellationToken cancellationToken)
    {
        var final = Path.Combine(Path.GetFullPath(outputRoot), workshopId.ToString());
        if (!ActiveInstallations.TryAdd(final, 0)) throw new ServiceException("DUPLICATE_DOWNLOAD", "This Workshop installation already has an active download.");
        try
        {
            const int maxAttempts = 4;
            for (var attempt = 1; ; attempt++)
            {
                try
                {
                    await session.WaitForOnlineAsync(cancellationToken).ConfigureAwait(false);
                    return await DownloadOnceAsync(taskId, workshopId, outputRoot, cancellationToken).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    throw;
                }
                catch (Exception error) when (attempt < maxAttempts && IsRetryable(error))
                {
                    writer.DownloadState(taskId, "resolving", message: "Retrying Steam download request.", errorCode: "DOWNLOAD_RETRYING");
                    await session.WaitForOnlineAsync(cancellationToken).ConfigureAwait(false);
                    await Task.Delay(TimeSpan.FromMilliseconds(500 * attempt * attempt), cancellationToken).ConfigureAwait(false);
                }
            }
        }
        finally
        {
            ActiveInstallations.TryRemove(final, out _);
        }
    }

    private async Task<DownloadResult> DownloadOnceAsync(string taskId, ulong workshopId, string outputRoot, CancellationToken cancellationToken)
    {
        if (!(OperatingSystem.IsMacOS() || OperatingSystem.IsLinux() || OperatingSystem.IsWindows())) throw new PlatformNotSupportedException();
        writer.DownloadState(taskId, "resolving");
        var detailsRequest = new CPublishedFile_GetDetails_Request { appid = SteamSession.AppId, includechildren = true };
        detailsRequest.publishedfileids.Add(workshopId);
        cancellationToken.ThrowIfCancellationRequested();
        var detailsResponse = await session.PublishedFiles.GetDetails(detailsRequest).ToTask().WaitAsync(cancellationToken).ConfigureAwait(false);
        var details = detailsResponse.Body.publishedfiledetails.FirstOrDefault(item => item.publishedfileid == workshopId);
        if (details == null || details.result != (uint)EResult.OK) throw new ServiceException("WORKSHOP_DETAILS_UNAVAILABLE", "Steam returned no valid Workshop item details.");
        if (details.consumer_appid != SteamSession.AppId) throw new ServiceException("WRONG_APP", "The Workshop item does not belong to Wallpaper Engine.");
        if (details.hcontent_file == 0) throw new ServiceException("CONTENT_MANIFEST_MISSING", "The Workshop item has no content manifest.");

        var appInfo = await session.GetAppInfoAsync(cancellationToken).ConfigureAwait(false);
        var depotId = appInfo.KeyValues["depots"]["workshopdepot"].AsUnsignedInteger();
        if (depotId == 0) throw new ServiceException("WORKSHOP_DEPOT_MISSING", "Wallpaper Engine Workshop depot could not be resolved.");
        cancellationToken.ThrowIfCancellationRequested();
        var depotKeyResult = await session.Apps.GetDepotDecryptionKey(depotId, SteamSession.AppId).ToTask().WaitAsync(cancellationToken).ConfigureAwait(false);
        if (depotKeyResult.Result != EResult.OK) throw new ServiceException("CONTENT_ACCESS_DENIED", depotKeyResult.Result.ToString());
        var requestCode = await session.Content.GetManifestRequestCode(depotId, SteamSession.AppId, details.hcontent_file, "public").WaitAsync(cancellationToken).ConfigureAwait(false);
        if (requestCode == 0) throw new ServiceException("MANIFEST_ACCESS_DENIED", "Steam returned no manifest request code.");

        using var cdn = new Client(session.Client);
        using var cancellationRegistration = cancellationToken.Register(cdn.Dispose);
        DepotManifest? manifest = null;
        Exception? manifestError = null;
        for (var attempt = 0; attempt < 4; attempt++)
        {
            string? serverHost = null;
            try
            {
                var serverInfo = await session.GetDownloadServerAsync(depotId, attempt, cancellationToken).ConfigureAwait(false);
                serverHost = serverInfo.Server.Host;
                manifest = await cdn.DownloadManifestAsync(depotId, details.hcontent_file, requestCode, serverInfo.Server, depotKeyResult.DepotKey, cdnAuthToken: serverInfo.Token).WaitAsync(cancellationToken).ConfigureAwait(false);
                session.ReportDownloadServerSuccess(serverHost);
                break;
            }
            catch (OperationCanceledException) { throw; }
            catch (Exception error) when (IsRetryable(error))
            {
                session.ReportDownloadServerFailure(depotId, serverHost);
                manifestError = error;
                await Task.Delay(TimeSpan.FromMilliseconds(300 * (attempt + 1)), cancellationToken).ConfigureAwait(false);
            }
        }
        if (manifest == null)
        {
            throw new ServiceException("MANIFEST_DOWNLOAD_FAILED", manifestError?.Message ?? "Steam did not return a content manifest.");
        }
        if (manifest.Files == null || manifest.Files.Count == 0) throw new ServiceException("EMPTY_MANIFEST", "The Workshop content manifest is empty.");

        var root = Path.GetFullPath(outputRoot);
        var stagingPath = Path.Combine(root, ".staging", workshopId.ToString());
        var manifestPaths = WorkshopStorage.ResolveManifestPaths(stagingPath, manifest.Files);
        var files = manifest.Files.Where(file => !file.Flags.HasFlag(EDepotFileFlag.Directory)).ToArray();
        foreach (var file in files) ValidateChunkLayout(file);
        var paths = WorkshopStorage.Prepare(root, workshopId, cancellationToken);
        var staging = paths.Staging;
        var final = paths.Final;
        var resolvedFiles = files.Select(file => (File: file, Path: manifestPaths[file.FileName])).ToArray();

        var needed = new ConcurrentQueue<(DepotManifest.FileData File, DepotManifest.ChunkData Chunk, string Path)>();
        long totalCompressed = 0;
        long reusableCompressed = 0;
        foreach (var entry in resolvedFiles)
        {
            cancellationToken.ThrowIfCancellationRequested();
            WorkshopStorage.CreateDirectory(Path.GetDirectoryName(entry.Path)!);
            WorkshopStorage.ValidateAncestors(entry.Path);
            // Truncating or extending a pre-existing name must never touch a
            // second directory entry that shares the same file content.
            if (File.Exists(entry.Path)) WorkshopStorage.ValidatePathIsNotLink(entry.Path);
            var existingLength = File.Exists(entry.Path) ? new FileInfo(entry.Path).Length : -1;
            if (existingLength != (long)entry.File.TotalSize)
            {
                using var resize = new FileStream(entry.Path, FileMode.OpenOrCreate, FileAccess.Write, FileShare.ReadWrite);
                resize.SetLength((long)entry.File.TotalSize);
            }
            using var validationStream = new FileStream(entry.Path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            foreach (var chunk in entry.File.Chunks.OrderBy(chunk => chunk.Offset))
            {
                totalCompressed += chunk.CompressedLength;
                if (IsChunkValid(validationStream, chunk)) reusableCompressed += chunk.CompressedLength;
                else needed.Enqueue((entry.File, chunk, entry.Path));
            }
        }

        using var progress = new DownloadProgressTracker(writer, taskId);
        progress.Start(totalCompressed, reusableCompressed);
        var workers = Enumerable.Range(0, Math.Min(8, Math.Max(1, needed.Count)))
            .Select(index => RunWorkerAsync(index, cdn, depotId, depotKeyResult.DepotKey, needed, progress, cancellationToken))
            .ToArray();
        await Task.WhenAll(workers).ConfigureAwait(false);
        progress.Stop();

        writer.DownloadState(taskId, "validating", totalCompressed, totalCompressed);
        WorkshopStorage.ValidateNoLinks(staging, cancellationToken);
        foreach (var entry in resolvedFiles)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ValidateFile(entry.File, entry.Path, cancellationToken);
            ApplyExecutableMode(entry.File, entry.Path);
        }

        WorkshopStorage.RemoveUnlistedFiles(staging, resolvedFiles.Select(item => item.Path).ToHashSet(WorkshopStorage.PathComparer), cancellationToken);
        var project = Path.Combine(staging, "project.json");
        if (!File.Exists(project)) throw new ServiceException("PROJECT_JSON_MISSING", "Downloaded content does not contain project.json.");
        WorkshopStorage.Install(staging, final, cancellationToken);
        return new DownloadResult(totalCompressed, final);
    }

    private async Task RunWorkerAsync(int workerIndex, Client cdn, uint depotId, byte[] depotKey, ConcurrentQueue<(DepotManifest.FileData File, DepotManifest.ChunkData Chunk, string Path)> queue, DownloadProgressTracker progress, CancellationToken cancellationToken)
    {
        while (queue.TryDequeue(out var work))
        {
            cancellationToken.ThrowIfCancellationRequested();
            await GlobalChunkSlots.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                Exception? lastError = null;
                for (var attempt = 0; attempt < 4; attempt++)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    string? serverHost = null;
                    try
                    {
                        var server = await session.GetDownloadServerAsync(depotId, workerIndex + attempt, cancellationToken).ConfigureAwait(false);
                        serverHost = server.Server.Host;
                        var buffer = new byte[work.Chunk.UncompressedLength];
                        using var transfer = progress.BeginTransfer(work.Chunk.CompressedLength);
                        DownloadContext.Current.Value = transfer;
                        try
                        {
                            var written = await cdn.DownloadDepotChunkAsync(depotId, work.Chunk, server.Server, buffer, depotKey, cdnAuthToken: server.Token).WaitAsync(cancellationToken).ConfigureAwait(false);
                            if (written != work.Chunk.UncompressedLength) throw new ServiceException("VALIDATION_FAILED", work.File.FileName);
                            WorkshopStorage.ValidateAncestors(work.Path);
                            using var handle = File.OpenHandle(work.Path, FileMode.Open, FileAccess.Write, FileShare.ReadWrite, FileOptions.Asynchronous | FileOptions.RandomAccess);
                            await RandomAccess.WriteAsync(handle, buffer.AsMemory(0, written), (long)work.Chunk.Offset, cancellationToken).ConfigureAwait(false);
                            transfer.Complete();
                            session.ReportDownloadServerSuccess(server.Server.Host);
                            lastError = null;
                            break;
                        }
                        finally
                        {
                            DownloadContext.Current.Value = null;
                        }
                    }
                    catch (OperationCanceledException) { throw; }
                    catch (Exception error)
                    {
                        session.ReportDownloadServerFailure(depotId, serverHost);
                        lastError = error;
                        await Task.Delay(TimeSpan.FromMilliseconds(300 * (attempt + 1)), cancellationToken).ConfigureAwait(false);
                    }
                }
                if (lastError != null) throw new ServiceException("CHUNK_DOWNLOAD_FAILED", lastError.Message);
            }
            finally
            {
                GlobalChunkSlots.Release();
            }
        }
    }

    private static bool IsRetryable(Exception error)
    {
        while (error.InnerException != null && error is AggregateException or IOException)
        {
            error = error.InnerException;
        }
        if (error is ServiceException service)
        {
            return service.Code is "CONNECTION_LOST" or "NO_CONTENT_SERVER" or "CHUNK_DOWNLOAD_FAILED";
        }
        return error is AsyncJobFailedException or SteamKitWebRequestException or HttpRequestException or IOException or TimeoutException or TaskCanceledException;
    }

    private static bool IsChunkValid(FileStream stream, DepotManifest.ChunkData chunk)
    {
        if (chunk.UncompressedLength == 0) return true;
        var length = (ulong)stream.Length;
        if (chunk.Offset > length || chunk.UncompressedLength > length - chunk.Offset) return false;
        var buffer = new byte[chunk.UncompressedLength];
        stream.Position = (long)chunk.Offset;
        var read = 0;
        while (read < buffer.Length)
        {
            var count = stream.Read(buffer, read, buffer.Length - read);
            if (count == 0) return false;
            read += count;
        }
        return DepotChunk.AdlerHash(buffer) == chunk.Checksum;
    }

    internal static void ValidateChunkLayout(DepotManifest.FileData file)
    {
        if (file.TotalSize > long.MaxValue) throw new ServiceException("INVALID_MANIFEST", "The content manifest contains an oversized file.");
        ulong offset = 0;
        foreach (var chunk in file.Chunks.OrderBy(chunk => chunk.Offset))
        {
            if (chunk.Offset != offset || chunk.UncompressedLength > int.MaxValue || chunk.UncompressedLength > file.TotalSize - offset)
                throw new ServiceException("INVALID_MANIFEST", "The content manifest contains invalid chunk ranges.");
            offset += chunk.UncompressedLength;
        }
        if (offset != file.TotalSize) throw new ServiceException("INVALID_MANIFEST", "The content manifest does not cover the complete file.");
    }

    internal static void ValidateFile(DepotManifest.FileData file, string path, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        WorkshopStorage.ValidateAncestors(path);
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if ((ulong)stream.Length != file.TotalSize) throw new ServiceException("VALIDATION_FAILED", file.FileName);
        foreach (var chunk in file.Chunks)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!IsChunkValid(stream, chunk)) throw new ServiceException("VALIDATION_FAILED", file.FileName);
        }
        if (file.FileHash is { Length: > 0 })
        {
            stream.Position = 0;
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA1);
            var buffer = new byte[64 * 1024];
            int read;
            while ((read = stream.Read(buffer)) != 0)
            {
                cancellationToken.ThrowIfCancellationRequested();
                hash.AppendData(buffer.AsSpan(0, read));
            }
            cancellationToken.ThrowIfCancellationRequested();
            if (!hash.GetHashAndReset().AsSpan().SequenceEqual(file.FileHash)) throw new ServiceException("VALIDATION_FAILED", file.FileName);
        }
    }

    internal static void ApplyExecutableMode(DepotManifest.FileData file, string path)
    {
        if (!OperatingSystem.IsWindows() && file.Flags.HasFlag(EDepotFileFlag.Executable))
        {
            var mode = File.GetUnixFileMode(path);
            File.SetUnixFileMode(path, mode | UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute);
        }
    }
}
