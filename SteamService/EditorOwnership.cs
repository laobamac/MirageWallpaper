//
//  EditorOwnership.cs
//  Mirage Wallpaper
//
//  Created on 2026/10/28.
//  Copyright © 2026 王孝慈. All rights reserved.
//

using SteamKit2;

namespace MirageSteamService;

internal sealed partial class SteamSession
{
    private TaskCompletionSource<SteamApps.LicenseListCallback> licenseSource = NewLicenseSource();
    private long licenseEpoch;
    public long LicenseEpoch => Interlocked.Read(ref licenseEpoch);

    private static TaskCompletionSource<SteamApps.LicenseListCallback> NewLicenseSource() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private void ResetEditorLicenses()
    {
        Interlocked.Increment(ref licenseEpoch);
        var previous = licenseSource;
        licenseSource = NewLicenseSource();
        previous.TrySetCanceled();
    }

    private void ReceiveEditorLicenses(SteamApps.LicenseListCallback callback)
    {
        Interlocked.Increment(ref licenseEpoch);
        var previous = licenseSource;
        var next = NewLicenseSource();
        next.SetResult(callback);
        licenseSource = next;
        previous.TrySetResult(callback);
    }

    internal static bool IsPersonalLicense(SteamApps.LicenseListCallback.License license, uint accountId)
    {
        const ELicenseFlags excluded = ELicenseFlags.Borrowed | ELicenseFlags.NonPermanent |
            ELicenseFlags.Expired | ELicenseFlags.Pending | ELicenseFlags.NotActivated |
            ELicenseFlags.CancelledByUser | ELicenseFlags.CancelledByAdmin |
            ELicenseFlags.CancelledByFriendlyFraudLock | ELicenseFlags.CancelledByPartner;
        return (license.OwnerAccountID == 0 || license.OwnerAccountID == accountId) &&
            (license.LicenseFlags & excluded) == 0 && license.LicenseType == ELicenseType.SinglePurchase;
    }

    public async Task<string> RequireEditorOwnershipAsync(CancellationToken cancellationToken)
    {
        if (!IsLoggedIn || !ulong.TryParse(SteamId, out var id))
            throw new ServiceException("NOT_AUTHENTICATED", "Sign in to Steam to open the editor.");
        var identity = SteamId;
        var licenses = await licenseSource.Task.WaitAsync(TimeSpan.FromSeconds(30), cancellationToken).ConfigureAwait(false);
        var epoch = LicenseEpoch;
        if (licenses.Result != EResult.OK)
            throw new ServiceException("OWNERSHIP_UNAVAILABLE", "Steam could not verify your licenses.");
        var candidates = licenses.LicenseList.Where(item => IsPersonalLicense(item, new SteamID(id).AccountID)).ToArray();
        var unresolved = false;
        foreach (var batch in candidates.Chunk(100))
        {
            var requests = batch.Select(item => new SteamApps.PICSRequest(item.PackageID, item.AccessToken)).ToArray();
            var response = await apps.PICSGetProductInfo([], requests).ToTask().WaitAsync(cancellationToken).ConfigureAwait(false);
            var packages = (response.Results ?? throw new ServiceException("OWNERSHIP_UNAVAILABLE", "Steam returned no licenses.")).SelectMany(result => result.Packages).ToDictionary(pair => pair.Key, pair => pair.Value);
            unresolved |= batch.Any(item => !packages.ContainsKey(item.PackageID));
            foreach (var package in packages.Values)
            {
                if (!package.KeyValues["appids"].Children.Any(app => app.AsUnsignedInteger() == AppId)) continue;
                EnsureEditorAccount(identity, epoch);
                return identity;
            }
        }
        EnsureEditorAccount(identity, epoch);
        throw new ServiceException(unresolved ? "OWNERSHIP_UNAVAILABLE" : "WE_NOT_OWNED",
            unresolved ? "Steam did not return all account licenses." : "This account does not own a permanent Wallpaper Engine license.");
    }

    public void EnsureEditorAccount(string identity, long epoch)
    {
        if (!IsLoggedIn || SteamId != identity || LicenseEpoch != epoch)
            throw new ServiceException("EDITOR_ACCOUNT_CHANGED", "The Steam session changed. Please verify ownership again.");
    }
}
