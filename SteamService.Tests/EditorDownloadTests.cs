//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using MirageSteamService;
using SteamKit2;
using SteamKit2.CDN;

internal static class EditorDownloadTests
{
    private const int ChunkSize = 8192;

    public static async Task RunAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "mirage-editor-download-" + Guid.NewGuid());
        Directory.CreateDirectory(root);
        try
        {
            FileIntegrity();
            await ParallelAndCacheReuseAsync(root);
            await CancellationAsync(root);
            await ValidationAsync(root);
        }
        finally { Directory.Delete(root, true); }
        Console.WriteLine("Editor parallel chunks, compatibility barrier, integrity, cancellation/resume passed.");
    }

    private static void FileIntegrity()
    {
        using (var empty = new MemoryStream())
        {
            if (!EditorDownloadQueue.IsFileValid(empty, 0, new byte[20])) throw new Exception("Empty depot file with unset hash rejected");
            if (!EditorDownloadQueue.IsFileValid(empty, 0, SHA1.HashData([]))) throw new Exception("Empty depot file with SHA-1 rejected");
            if (EditorDownloadQueue.IsFileValid(empty, 1, new byte[20])) throw new Exception("Truncated file accepted");
        }
        using (var data = new MemoryStream([1, 2, 3]))
        {
            if (!EditorDownloadQueue.IsFileValid(data, 3, SHA1.HashData(new byte[] { 1, 2, 3 }))) throw new Exception("Valid depot file rejected");
            if (EditorDownloadQueue.IsFileValid(data, 3, new byte[20])) throw new Exception("Corrupt depot file accepted");
            if (EditorDownloadQueue.IsFileValid(data, 0, new byte[20])) throw new Exception("Nonempty content accepted for empty depot file");
        }
    }

    private static (EditorDownloadFile File, byte[] Data) Fixture(string root, string name, int chunks, bool required = false)
    {
        var data = new byte[chunks * ChunkSize];
        for (var i = 0; i < data.Length; i++) data[i] = (byte)(i * 17 + i / ChunkSize + name.Length);
        var manifest = new DepotManifest.FileData(name, SHA1.HashData(Encoding.UTF8.GetBytes(name)), (EDepotFileFlag)0,
            (ulong)data.Length, SHA1.HashData(data), "", false, chunks);
        for (var i = 0; i < chunks; i++)
        {
            var bytes = data.AsSpan(i * ChunkSize, ChunkSize);
            manifest.Chunks.Add(new DepotManifest.ChunkData(SHA1.HashData(bytes), DepotChunk.AdlerHash(bytes),
                (ulong)(i * ChunkSize), ChunkSize, ChunkSize));
        }
        return (new EditorDownloadFile(1, [], manifest, name, Path.Combine(root, name),
            required ? Convert.ToHexString(SHA256.HashData(data)) : null), data);
    }

    private static async Task WriteAsync(EditorDownloadFile file, DepotManifest.ChunkData chunk, byte[] data, CancellationToken token)
    {
        using var handle = File.OpenHandle(file.Path, FileMode.Open, FileAccess.Write, FileShare.ReadWrite, FileOptions.Asynchronous);
        await RandomAccess.WriteAsync(handle, data.AsMemory((int)chunk.Offset, (int)chunk.UncompressedLength), (long)chunk.Offset, token);
    }

    private static async Task ParallelAndCacheReuseAsync(string root)
    {
        var fixtures = new[] { Fixture(root, "required-a", 12, true), Fixture(root, "required-b", 12, true),
            Fixture(root, "large", 40), Fixture(root, "empty", 0) }
            .Concat(Enumerable.Range(0, 24).Select(i => Fixture(root, "small-" + i, 1))).ToArray();
        var data = fixtures.ToDictionary(item => item.File.Path, item => item.Data);
        var slotsFilled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var activeNames = new ConcurrentDictionary<string, byte>();
        int active = 0, peak = 0, requiredFinished = 0;
        using var progress = new DownloadProgressTracker(new ProtocolWriter(TextWriter.Null), "parallel-test");
        var queue = new EditorDownloadQueue(progress, async (file, chunk, token) =>
        {
            if (file.CompatibilityHash == null && Volatile.Read(ref requiredFinished) != 24)
                throw new Exception("Bulk files started before compatibility files completed");
            var count = Interlocked.Increment(ref active);
            int previous;
            do
            {
                previous = Volatile.Read(ref peak);
            } while (count > previous && Interlocked.CompareExchange(ref peak, count, previous) != previous);
            activeNames.TryAdd(file.InstallPath, 0);
            using var transfer = progress.BeginTransfer(chunk.CompressedLength, file.InstallPath);
            try
            {
                if (count == EditorDownloadQueue.MaxConcurrentChunks) slotsFilled.TrySetResult();
                await slotsFilled.Task.WaitAsync(TimeSpan.FromSeconds(5), token);
                await WriteAsync(file, chunk, data[file.Path], token);
                transfer.AddNetworkBytes((int)chunk.CompressedLength);
                transfer.Complete();
                if (file.CompatibilityHash != null) Interlocked.Increment(ref requiredFinished);
            }
            finally { Interlocked.Decrement(ref active); }
        });
        await queue.RunAsync(fixtures.Select(item => item.File).ToArray(), CancellationToken.None);
        progress.Stop();
        if (peak != EditorDownloadQueue.MaxConcurrentChunks || active != 0 || activeNames.Count != fixtures.Length - 1)
            throw new Exception("Chunk queue did not keep its bounded parallelism across files");
        foreach (var fixture in fixtures)
            if (!File.ReadAllBytes(fixture.File.Path).SequenceEqual(fixture.Data)) throw new Exception("Parallel writes corrupted file");

        using var reused = new DownloadProgressTracker(new ProtocolWriter(TextWriter.Null), "reuse-test");
        var cached = new EditorDownloadQueue(reused, (_, _, _) => throw new Exception("Valid chunk redownloaded"));
        await cached.RunAsync(fixtures.Select(item => item.File).ToArray(), CancellationToken.None);
        reused.Stop();

        int requests = 0;
        var damaged = fixtures[2];
        using (var stream = File.OpenWrite(damaged.File.Path)) { stream.Position = ChunkSize + 10; stream.WriteByte(0); }
        using var repairProgress = new DownloadProgressTracker(new ProtocolWriter(TextWriter.Null), "repair-test");
        var repaired = new EditorDownloadQueue(repairProgress, async (file, chunk, ct) =>
        {
            if (file.Path != damaged.File.Path || chunk.Offset != ChunkSize) throw new Exception("Undamaged chunk redownloaded");
            Interlocked.Increment(ref requests);
            await WriteAsync(file, chunk, data[file.Path], ct);
        });
        await repaired.RunAsync(fixtures.Select(item => item.File).ToArray(), CancellationToken.None);
        repairProgress.Stop();
        if (requests != 1) throw new Exception("Damaged chunk was not repaired");
    }

    private static async Task CancellationAsync(string root)
    {
        var fixture = Fixture(root, "cancel", 64);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        using var progress = new DownloadProgressTracker(new ProtocolWriter(TextWriter.Null), "cancel-test");
        int active = 0, started = 0;
        var queue = new EditorDownloadQueue(progress, async (file, chunk, ct) =>
        {
            Interlocked.Increment(ref active);
            Interlocked.Increment(ref started);
            try
            {
                if (chunk.Offset == 0)
                {
                    await WriteAsync(file, chunk, fixture.Data, ct);
                    cancellation.Cancel();
                }
                await Task.Delay(Timeout.Infinite, ct);
            }
            finally { Interlocked.Decrement(ref active); }
        });
        try { await queue.RunAsync([fixture.File], cancellation.Token); throw new Exception("Cancellation ignored"); }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { }
        if (active != 0 || started > EditorDownloadQueue.MaxConcurrentChunks) throw new Exception("Cancellation leaked workers");
        using var resumed = new DownloadProgressTracker(new ProtocolWriter(TextWriter.Null), "resume-test");
        int requests = 0;
        var resume = new EditorDownloadQueue(resumed, async (file, chunk, ct) =>
        {
            if (chunk.Offset == 0) throw new Exception("Completed chunk discarded on cancellation");
            Interlocked.Increment(ref requests);
            await WriteAsync(file, chunk, fixture.Data, ct);
        });
        await resume.RunAsync([fixture.File], CancellationToken.None);
        resumed.Stop();
        if (requests != 63) throw new Exception("Resume did not reuse completed chunks");
    }

    private static async Task ValidationAsync(string root)
    {
        var required = Fixture(root, "incompatible", 2, true);
        var bulk = Fixture(root, "must-not-download", 4);
        using var progress = new DownloadProgressTracker(new ProtocolWriter(TextWriter.Null), "compatibility-test");
        var queue = new EditorDownloadQueue(progress, async (file, chunk, ct) =>
        {
            if (file.CompatibilityHash == null) throw new Exception("Downloaded bulk content for an incompatible version");
            await WriteAsync(file, chunk, required.Data, ct);
        });
        try
        {
            await queue.RunAsync([required.File with { CompatibilityHash = new string('0', 64) }, bulk.File], CancellationToken.None);
            throw new Exception("Incompatible editor accepted");
        }
        catch (ServiceException error) when (error.Code == "EDITOR_VERSION_UNAVAILABLE") { }

        var corrupt = Fixture(root, "corrupt", 2);
        using var corruptProgress = new DownloadProgressTracker(new ProtocolWriter(TextWriter.Null), "integrity-test");
        var invalid = new EditorDownloadQueue(corruptProgress, (file, chunk, ct) => WriteAsync(file, chunk, new byte[corrupt.Data.Length], ct));
        try { await invalid.RunAsync([corrupt.File], CancellationToken.None); throw new Exception("Corrupt file accepted"); }
        catch (ServiceException error) when (error.Code == "VALIDATION_FAILED") { }

        var failing = Fixture(root, "failed-worker", 64);
        using var failedProgress = new DownloadProgressTracker(new ProtocolWriter(TextWriter.Null), "worker-failure-test");
        int active = 0;
        var failure = new EditorDownloadQueue(failedProgress, async (_, chunk, ct) =>
        {
            Interlocked.Increment(ref active);
            try
            {
                if (chunk.Offset == 0) throw new IOException("Simulated transfer failure");
                await Task.Delay(Timeout.Infinite, ct);
            }
            finally { Interlocked.Decrement(ref active); }
        });
        try { await failure.RunAsync([failing.File], CancellationToken.None).WaitAsync(TimeSpan.FromSeconds(5)); throw new Exception("Worker failure ignored"); }
        catch (IOException) { }
        if (active != 0) throw new Exception("Worker failure left transfers running");
    }

}
