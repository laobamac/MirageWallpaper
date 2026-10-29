//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

using System.Net;

namespace MirageSteamService;

internal static class DownloadContext
{
    public static readonly AsyncLocal<DownloadTransfer?> Current = new();
}

internal sealed class DownloadProgressTracker : IDisposable
{
    private readonly ProtocolWriter writer;
    private readonly string taskId;
    private readonly object gate = new();
    private readonly Timer timer;
    private readonly Queue<(DateTime Time, long Bytes)> samples = new();
    private long networkBytes;
    private long reusableBytes;
    private long completedBytes;
    private long totalBytes;
    private int totalFiles;
    private int completedFiles;
    private readonly HashSet<DownloadTransfer> transfers = [];
    private bool active;
    public string? CurrentFile { get; set; }

    public DownloadProgressTracker(ProtocolWriter writer, string taskId)
    {
        this.writer = writer;
        this.taskId = taskId;
        timer = new Timer(_ => Emit(), null, Timeout.Infinite, Timeout.Infinite);
    }

    public void Start(long total, long reusable, int fileCount = 0)
    {
        lock (gate)
        {
            totalBytes = Math.Max(total, 0);
            reusableBytes = Math.Clamp(reusable, 0, totalBytes);
            networkBytes = 0;
            completedBytes = 0;
            totalFiles = fileCount;
            completedFiles = 0;
            transfers.Clear();
            samples.Clear();
            samples.Enqueue((DateTime.UtcNow, 0));
            active = true;
        }
        timer.Change(0, 500);
    }

    public void CompleteFile()
    {
        lock (gate) completedFiles++;
    }

    public DownloadTransfer BeginTransfer(long expectedBytes, string? fileName = null)
    {
        var transfer = new DownloadTransfer(this, expectedBytes, fileName);
        lock (gate)
        {
            transfers.Add(transfer);
            if (fileName != null) CurrentFile = fileName;
        }
        return transfer;
    }

    internal void AddNetworkBytes(DownloadTransfer transfer, int count)
    {
        if (count <= 0) return;
        lock (gate)
        {
            networkBytes += count;
            transfer.Add(count);
            samples.Enqueue((DateTime.UtcNow, networkBytes));
        }
    }

    internal void FinishTransfer(DownloadTransfer transfer, bool completed)
    {
        lock (gate)
        {
            transfers.Remove(transfer);
            if (completed) completedBytes += transfer.ExpectedBytes;
        }
    }

    public void Stop()
    {
        lock (gate) active = false;
        timer.Change(Timeout.Infinite, Timeout.Infinite);
        Emit(true);
    }

    private void Emit(bool force = false)
    {
        long received;
        long total;
        double speed;
        int files, completed, activeFiles, activeChunks;
        string? currentFile;
        lock (gate)
        {
            if (!active && !force) return;
            var now = DateTime.UtcNow;
            while (samples.Count > 1 && now - samples.Peek().Time > TimeSpan.FromSeconds(5)) samples.Dequeue();
            var first = samples.Peek();
            var elapsed = Math.Max((now - first.Time).TotalSeconds, 0.001);
            speed = (networkBytes - first.Bytes) / elapsed;
            var inFlight = transfers.Sum(transfer => Math.Min(transfer.ReceivedBytes, transfer.ExpectedBytes));
            received = Math.Min(totalBytes, reusableBytes + completedBytes + inFlight);
            total = totalBytes;
            files = totalFiles;
            completed = completedFiles;
            activeFiles = transfers.Where(transfer => transfer.FileName != null).Select(transfer => transfer.FileName).Distinct().Count();
            activeChunks = transfers.Count;
            currentFile = transfers.FirstOrDefault(transfer => transfer.FileName != null)?.FileName ?? CurrentFile;
            var remaining = Math.Max(0, total - received);
            double? eta = speed > 1 && remaining > 0 ? remaining / speed : null;
            // Keep timer snapshots ordered with Stop so an older callback cannot overwrite final progress.
            writer.DownloadState(taskId, "downloading", received, total, speed, eta, currentFile: currentFile,
                totalFiles: files, completedFiles: completed, activeFiles: activeFiles, activeChunks: activeChunks);
        }
    }

    public void Dispose()
    {
        lock (gate) active = false;
        timer.Dispose();
    }
}

internal sealed class DownloadTransfer : IDisposable
{
    private readonly DownloadProgressTracker tracker;
    private bool completed;
    private bool disposed;

    public long ExpectedBytes { get; }
    public string? FileName { get; }
    public long ReceivedBytes { get; private set; }

    public DownloadTransfer(DownloadProgressTracker tracker, long expectedBytes, string? fileName = null)
    {
        this.tracker = tracker;
        ExpectedBytes = Math.Max(0, expectedBytes);
        FileName = fileName;
    }

    public void AddNetworkBytes(int count)
    {
        tracker.AddNetworkBytes(this, count);
    }

    internal void Add(int count)
    {
        ReceivedBytes += count;
    }

    public void Complete()
    {
        completed = true;
    }

    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        tracker.FinishTransfer(this, completed);
    }
}

internal sealed class CountingHandler : DelegatingHandler
{
    public CountingHandler(HttpMessageHandler inner) : base(inner) { }

    protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        var tracker = DownloadContext.Current.Value;
        var response = await base.SendAsync(request, cancellationToken).ConfigureAwait(false);
        if (tracker != null && response.Content != null)
        {
            response.Content = new CountingContent(response.Content, tracker);
        }
        return response;
    }
}

internal sealed class CountingContent : HttpContent
{
    private readonly HttpContent source;
    private readonly DownloadTransfer transfer;

    public CountingContent(HttpContent source, DownloadTransfer transfer)
    {
        this.source = source;
        this.transfer = transfer;
        foreach (var header in source.Headers) Headers.TryAddWithoutValidation(header.Key, header.Value);
    }

    protected override bool TryComputeLength(out long length)
    {
        length = source.Headers.ContentLength ?? -1;
        return length >= 0;
    }

    protected override async Task SerializeToStreamAsync(Stream stream, TransportContext? context)
    {
        await CopyAsync(stream, CancellationToken.None).ConfigureAwait(false);
    }

    protected override async Task SerializeToStreamAsync(Stream stream, TransportContext? context, CancellationToken cancellationToken)
    {
        await CopyAsync(stream, cancellationToken).ConfigureAwait(false);
    }

    private async Task CopyAsync(Stream destination, CancellationToken cancellationToken)
    {
        await using var input = await source.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        var buffer = new byte[64 * 1024];
        while (true)
        {
            var read = await input.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);
            if (read == 0) break;
            await destination.WriteAsync(buffer.AsMemory(0, read), cancellationToken).ConfigureAwait(false);
            transfer.AddNetworkBytes(read);
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) source.Dispose();
        base.Dispose(disposing);
    }
}
