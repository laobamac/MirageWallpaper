//
//  Program.cs
//  Mirage Wallpaper
//
//  Created on 2026/10/28.
//  Copyright © 2026 王孝慈. All rights reserved.
//

using System.Reflection;
using System.Security.Cryptography;
using MirageSteamService;
using SteamKit2;
using SteamKit2.Internal;

static SteamApps.LicenseListCallback.License License(ELicenseFlags flags, uint owner = 42, ELicenseType type = ELicenseType.SinglePurchase) =>
    (SteamApps.LicenseListCallback.License)Activator.CreateInstance(typeof(SteamApps.LicenseListCallback.License),
        BindingFlags.Instance | BindingFlags.NonPublic, null,
        [new CMsgClientLicenseList.License { owner_id = owner, flags = (uint)flags, license_type = (uint)type }], null)!;

if (!SteamSession.IsPersonalLicense(License(ELicenseFlags.None), 42)) throw new Exception("Personal license rejected");
if (!SteamSession.IsPersonalLicense(License(ELicenseFlags.None, 0), 42)) throw new Exception("Own-account license without explicit owner rejected");
foreach (var flag in new[] { ELicenseFlags.Borrowed, ELicenseFlags.NonPermanent, ELicenseFlags.Expired,
    ELicenseFlags.CancelledByAdmin, ELicenseFlags.Pending, ELicenseFlags.NotActivated })
    if (SteamSession.IsPersonalLicense(License(flag), 42)) throw new Exception("Invalid entitlement accepted: " + flag);
if (SteamSession.IsPersonalLicense(License(ELicenseFlags.None, 99), 42)) throw new Exception("Another owner's license accepted");
if (SteamSession.IsPersonalLicense(License(ELicenseFlags.None, 42, ELicenseType.SinglePurchaseLimitedUse), 42)) throw new Exception("Trial license accepted");
if (EditorContentService.InstallPath("distribution\\bin\\wallpaperui.exe") != "bin/wallpaperui.exe") throw new Exception("Steam distribution layout was not installed");
if (EditorContentService.InstallPath("ui/dist/scripts/scripts.js") != "ui/dist/scripts/scripts.js") throw new Exception("Shared asset path was changed");
using (var empty = new MemoryStream())
{
    if (!EditorContentService.IsFileValid(empty, 0, new byte[20])) throw new Exception("Empty depot file with unset hash rejected");
    if (!EditorContentService.IsFileValid(empty, 0, SHA1.HashData([]))) throw new Exception("Empty depot file with SHA-1 rejected");
    if (EditorContentService.IsFileValid(empty, 1, new byte[20])) throw new Exception("Truncated file accepted");
}
using (var data = new MemoryStream([1, 2, 3]))
{
    if (!EditorContentService.IsFileValid(data, 3, SHA1.HashData(new byte[] { 1, 2, 3 }))) throw new Exception("Valid depot file rejected");
    if (EditorContentService.IsFileValid(data, 3, new byte[20])) throw new Exception("Corrupt depot file accepted");
    if (EditorContentService.IsFileValid(data, 0, new byte[20])) throw new Exception("Nonempty content accepted for empty depot file");
}
var root = Path.Combine(Path.GetTempPath(), "mirage-editor-path-tests-" + Guid.NewGuid());
foreach (var path in new[] { "../escape", "/tmp/escape", "folder/../../escape", "folder\\..\\..\\escape" })
{
    try { WorkshopDownloader.ResolveContainedPath(root, path); throw new Exception("Unsafe path accepted"); }
    catch (ServiceException error) when (error.Code == "UNSAFE_PATH") { }
}
await using var session = new SteamSession(new ProtocolWriter());
try { await session.RequireEditorOwnershipAsync(CancellationToken.None); throw new Exception("Unauthenticated account accepted"); }
catch (ServiceException error) when (error.Code == "NOT_AUTHENTICATED") { }
Console.WriteLine("Ownership classification, unauthenticated access, manifest path containment and file integrity passed.");
await EditorDownloadTests.RunAsync();
