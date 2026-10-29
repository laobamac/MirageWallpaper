//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

using System.Security.Cryptography;
using SteamKit2;

namespace MirageSteamService;

internal sealed record EditorDownloadFile(uint DepotId, byte[] DepotKey, DepotManifest.FileData Manifest,
    string InstallPath, string Path, string? CompatibilityHash);

/// <summary>A bounded chunk queue shared by all files, with a compatibility check before bulk downloading.</summary>
internal sealed class EditorDownloadQueue(DownloadProgressTracker progress,
    Func<EditorDownloadFile, DepotManifest.ChunkData, CancellationToken, Task> downloadChunk)
{
    internal const int MaxConcurrentChunks = 16;
    private sealed class PendingFile(EditorDownloadFile file, DepotManifest.ChunkData[] chunks)
    {
        public readonly EditorDownloadFile Entry = file;
        public readonly DepotManifest.ChunkData[] Chunks = chunks;
        public int RemainingChunks = chunks.Length;
    }

    public async Task RunAsync(IReadOnlyList<EditorDownloadFile> files, CancellationToken token)
    {
        var pending = new List<PendingFile>();
        long reusable = 0;
        foreach (var file in files)
        {
            token.ThrowIfCancellationRequested();
            Directory.CreateDirectory(Path.GetDirectoryName(file.Path)!);
            using var stream = new FileStream(file.Path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.Read);
            var existingLength = checked((ulong)stream.Length);
            stream.SetLength(checked((long)file.Manifest.TotalSize));
            var missing = new List<DepotManifest.ChunkData>();
            foreach (var chunk in file.Manifest.Chunks)
            {
                token.ThrowIfCancellationRequested();
                // A new or truncated file cannot contain these chunks. Avoid reading preallocated zeroes.
                if (chunk.Offset <= existingLength && chunk.UncompressedLength <= existingLength - chunk.Offset &&
                    WorkshopDownloader.IsChunkValid(stream, chunk)) reusable += chunk.CompressedLength;
                else missing.Add(chunk);
            }
            pending.Add(new PendingFile(file, missing.ToArray()));
        }

        progress.Start(files.Sum(file => file.Manifest.Chunks.Sum(chunk => (long)chunk.CompressedLength)), reusable, files.Count);
        await RunBatchAsync(pending.Where(file => file.Entry.CompatibilityHash != null), token).ConfigureAwait(false);
        await RunBatchAsync(pending.Where(file => file.Entry.CompatibilityHash == null), token).ConfigureAwait(false);
    }

    private async Task RunBatchAsync(IEnumerable<PendingFile> batch, CancellationToken token)
    {
        IEnumerable<(PendingFile File, DepotManifest.ChunkData Chunk)> Chunks()
        {
            foreach (var file in batch)
            {
                token.ThrowIfCancellationRequested();
                if (file.Chunks.Length == 0) ValidateFile(file.Entry, token);
                foreach (var chunk in file.Chunks) yield return (file, chunk);
            }
        }

        await Parallel.ForEachAsync(Chunks(),
            new ParallelOptions { MaxDegreeOfParallelism = MaxConcurrentChunks, CancellationToken = token },
            async (work, ct) =>
            {
                await downloadChunk(work.File.Entry, work.Chunk, ct).ConfigureAwait(false);
                if (Interlocked.Decrement(ref work.File.RemainingChunks) == 0) ValidateFile(work.File.Entry, ct);
            }).ConfigureAwait(false);
    }

    internal static bool IsFileValid(Stream stream, ulong expectedSize, byte[] expectedHash)
    {
        if (checked((ulong)stream.Length) != expectedSize) return false;
        // Empty depot files may have an unset SHA-1. Their length fully determines their contents.
        if (expectedSize == 0) return true;
        stream.Position = 0;
        return SHA1.HashData(stream).SequenceEqual(expectedHash);
    }

    private void ValidateFile(EditorDownloadFile file, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        using var stream = File.OpenRead(file.Path);
        if (!IsFileValid(stream, file.Manifest.TotalSize, file.Manifest.FileHash))
            throw new ServiceException("VALIDATION_FAILED", "Downloaded file failed verification: " + file.InstallPath);
        if (file.CompatibilityHash != null)
        {
            stream.Position = 0;
            if (!Convert.ToHexString(SHA256.HashData(stream)).Equals(file.CompatibilityHash, StringComparison.OrdinalIgnoreCase))
                throw new ServiceException("EDITOR_VERSION_UNAVAILABLE", "The version provided by Steam needs a newer Mirage editor adapter.");
        }
        progress.CompleteFile();
    }
}
