//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

using System.Text;
using System.Text.Json;
using MirageSteamService;

internal static class DownloadProgressTests
{
    public static async Task RunAsync()
    {
        ProgressPayload();
        await BlockedProgressOutputAsync();
        Console.WriteLine("Parallel progress reporting and blocked-output isolation passed.");
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
