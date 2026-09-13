//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

using System.Text.Json;
using System.Text.Json.Serialization;

namespace MirageSteamService;

internal sealed class ServiceCommand
{
    public string Command { get; set; } = "";
    public string? RequestId { get; set; }
    public string? TaskId { get; set; }
    public string? WorkshopId { get; set; }
    public string? Username { get; set; }
    public string? Password { get; set; }
    public string? RefreshToken { get; set; }
    public string? GuardData { get; set; }
    public string? Code { get; set; }
    public string? OutputRoot { get; set; }
    public List<string>? WorkshopIds { get; set; }
    public string? CreatorSteamId { get; set; }
    public string? Text { get; set; }
    public int? StartIndex { get; set; }
    public int? Count { get; set; }
    public uint? LoginId { get; set; }
}

internal sealed class ProtocolWriter
{
    private readonly object gate = new();
    private readonly JsonSerializerOptions options = new()
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    public void Send(object payload)
    {
        var json = JsonSerializer.Serialize(payload, options);
        lock (gate)
        {
            Console.Out.WriteLine(json);
            Console.Out.Flush();
        }
    }

    public void Response(string? requestId, bool success, string? message = null, string? errorCode = null, object? data = null)
    {
        Send(new { type = "response", requestId, success, message, errorCode, data });
    }

    public void AuthState(string state, string? accountName = null, string? message = null, string? challengeUrl = null, string? refreshToken = null, string? guardData = null, string? errorCode = null, string? steamId = null)
    {
        Send(new { type = "authState", state, accountName, message, challengeUrl, refreshToken, guardData, errorCode, steamId });
    }

    public void DownloadState(string taskId, string state, long receivedBytes = 0, long totalBytes = 0, double bytesPerSecond = 0, double? etaSeconds = null, string? outputPath = null, string? message = null, string? errorCode = null)
    {
        Send(new { type = "downloadState", taskId, state, receivedBytes, totalBytes, bytesPerSecond, etaSeconds, outputPath, message, errorCode });
    }
}
