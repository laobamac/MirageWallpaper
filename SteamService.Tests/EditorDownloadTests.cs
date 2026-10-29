//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
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
            await ParallelAndResumeAsync(root);
            await CancellationAsync(root);
            await ValidationAsync(root);
            ProgressPayload();
            await BlockedProgressOutputAsync();
        }
        finally { Directory.Delete(root, true); }
        Console.WriteLine("Editor parallel chunks, compatibility barrier, integrity, cancellation/resume and progress passed.");
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

    private static async Task ParallelAndResumeAsync(string root)
    {
        var fixtures = new[] { Fixture(root, "required-a", 12, true), Fixture(root, "required-b", 12, true),
            Fixture(root, "large", 40), Fixture(root, "empty", 0) }
            .Concat(Enumerable.Range(0, 24).Select(i => Fixture(root, "small-" + i, 1))).ToArray();
        var data = fixtures.ToDictionary(item => item.File.Path, item => item.Data);
        var slotsFilled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var activeNames = new ConcurrentDictionary<string, byte>();
        int active = 0, peak = 0, requiredFinished = 0, requests = 0;
        using var progress = new DownloadProgressTracker(new ProtocolWriter(), "parallel-test");
        var queue = new EditorDownloadQueue(progress, async (file, chunk, token) =>
        {
            Interlocked.Increment(ref requests);
            if (file.CompatibilityHash == null && Volatile.Read(ref requiredFinished) != 24)
                throw new Exception("Bulk files started before compatibility files completed");
            var count = Interlocked.Increment(ref active);
            int previous;
            do { previous = Volatile.Read(ref peak); } while (count > previous && Interlocked.CompareExchange(ref peak, count, previous) != previous);
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

        requests = 0;
        using var reused = new DownloadProgressTracker(new ProtocolWriter(), "reuse-test");
        var cached = new EditorDownloadQueue(reused, (_, _, _) => { Interlocked.Increment(ref requests); throw new Exception("Valid chunk redownloaded"); });
        await cached.RunAsync(fixtures.Select(item => item.File).ToArray(), CancellationToken.None);
        reused.Stop();
        if (requests != 0) throw new Exception("Cache reuse performed a download");

        var damaged = fixtures[2];
        using (var stream = File.OpenWrite(damaged.File.Path)) { stream.Position = ChunkSize + 10; stream.WriteByte(0); }
        using var repairProgress = new DownloadProgressTracker(new ProtocolWriter(), "repair-test");
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
        using var progress = new DownloadProgressTracker(new ProtocolWriter(), "cancel-test");
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
        using var resumed = new DownloadProgressTracker(new ProtocolWriter(), "resume-test");
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
        using var progress = new DownloadProgressTracker(new ProtocolWriter(), "compatibility-test");
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
        using var corruptProgress = new DownloadProgressTracker(new ProtocolWriter(), "integrity-test");
        var invalid = new EditorDownloadQueue(corruptProgress, (file, chunk, ct) => WriteAsync(file, chunk, new byte[corrupt.Data.Length], ct));
        try { await invalid.RunAsync([corrupt.File], CancellationToken.None); throw new Exception("Corrupt file accepted"); }
        catch (ServiceException error) when (error.Code == "VALIDATION_FAILED") { }

        var failing = Fixture(root, "failed-worker", 64);
        using var failedProgress = new DownloadProgressTracker(new ProtocolWriter(), "worker-failure-test");
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

    private static void ProgressPayload()
    {
        using var output = new StringWriter();
        using var progress = new DownloadProgressTracker(new ProtocolWriter(output), "progress-test");
        progress.Start(300, 0, 2);
        using var first = progress.BeginTransfer(100, "first");
        using var second = progress.BeginTransfer(100, "first");
        using var third = progress.BeginTransfer(100, "second");
        first.AddNetworkBytes(20);
        second.AddNetworkBytes(30);
        third.AddNetworkBytes(40);
        progress.Stop();
        using var json = JsonDocument.Parse(output.ToString().Trim().Split('\n')[^1]);
        var value = json.RootElement;
        if (value.GetProperty("activeFiles").GetInt32() != 2 || value.GetProperty("activeChunks").GetInt32() != 3 ||
            value.GetProperty("receivedBytes").GetInt64() != 90 || value.GetProperty("completedFiles").GetInt32() != 0)
            throw new Exception("Parallel progress did not report distinct files and in-flight chunks");
    }

    private sealed class BlockedWriter : TextWriter
    {
        public override Encoding Encoding => Encoding.UTF8;
        public readonly TaskCompletionSource Started = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public readonly ManualResetEventSlim Release = new();

        public override void WriteLine(string? value)
        {
            Started.TrySetResult();
            if (!Release.Wait(TimeSpan.FromSeconds(10))) throw new TimeoutException("Test output was not released");
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) Release.Dispose();
            base.Dispose(disposing);
        }
    }

    private static async Task BlockedProgressOutputAsync()
    {
        using var output = new BlockedWriter();
        using var progress = new DownloadProgressTracker(new ProtocolWriter(output), "blocked-output-test");
        progress.Start(100, 0, 1);
        Task? transferTask = null;
        try
        {
            await output.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            transferTask = Task.Factory.StartNew(() =>
            {
                using var transfer = progress.BeginTransfer(100, "file");
                transfer.AddNetworkBytes(100);
                transfer.Complete();
                progress.CompleteFile();
            }, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
            await transferTask.WaitAsync(TimeSpan.FromSeconds(2));
        }
        finally
        {
            output.Release.Set();
            if (transferTask != null) await transferTask;
            progress.Stop();
        }
    }
}
