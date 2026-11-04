using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using SteamKit2;

namespace MirageSteamService;

internal static class WorkshopStorage
{
    internal static StringComparer PathComparer => OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
    private static StringComparison PathComparison => OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;

    internal sealed record Paths(string Staging, string Final);

    internal static Paths Prepare(string outputRoot, ulong workshopId, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(outputRoot));
        CreateDirectory(root);
        var stagingParent = Path.Combine(root, ".staging");
        CreateDirectory(stagingParent);
        var staging = Path.Combine(stagingParent, workshopId.ToString());
        var final = Path.Combine(root, workshopId.ToString());
        var backup = final + ".previous";
        ValidateDirectoryIfPresent(final, cancellationToken);
        ValidateDirectoryIfPresent(backup, cancellationToken);
        ValidateDirectoryIfPresent(staging, cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();
        if (!Directory.Exists(final) && Directory.Exists(backup)) Directory.Move(backup, final);
        if (!Directory.Exists(staging))
        {
            if (Directory.Exists(final)) CopyDirectory(final, staging, cancellationToken);
            else CreateDirectory(staging);
        }
        ValidateNoLinks(staging, cancellationToken);
        return new Paths(staging, final);
    }

    internal static string ResolveContainedPath(string root, string relative)
    {
        if (string.IsNullOrWhiteSpace(relative)) throw UnsafePath("The content manifest contains an empty path.");
        var normalized = relative.Replace('\\', Path.DirectorySeparatorChar).Replace('/', Path.DirectorySeparatorChar);
        if (Path.IsPathRooted(normalized) || normalized.Contains(':')) throw UnsafePath("The content manifest contains an absolute or drive-qualified path.");
        var components = normalized.Split(Path.DirectorySeparatorChar);
        foreach (var component in components)
        {
            if (component.Length == 0 || component is "." or ".." || component.Contains('\0')) throw UnsafePath("The content manifest contains an invalid path component.");
            if (OperatingSystem.IsWindows())
            {
                if (component.EndsWith(' ') || component.EndsWith('.') || component.Any(character => character < 32 || "<>\"|?*".Contains(character)))
                    throw UnsafePath("The content manifest contains an invalid Windows filename.");
                var name = component.Split('.')[0].TrimEnd(' ').ToUpperInvariant();
                if (name is "CON" or "PRN" or "AUX" or "NUL" or "CONIN$" or "CONOUT$" ||
                    (name.Length == 4 && (name.StartsWith("COM", StringComparison.Ordinal) || name.StartsWith("LPT", StringComparison.Ordinal)) && "123456789¹²³".Contains(name[3])))
                    throw UnsafePath("The content manifest contains a reserved Windows filename.");
            }
        }
        var fullRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        var path = Path.GetFullPath(Path.Combine(fullRoot, normalized));
        var prefix = Path.EndsInDirectorySeparator(fullRoot) ? fullRoot : fullRoot + Path.DirectorySeparatorChar;
        if (!path.StartsWith(prefix, PathComparison) || PathComparer.Equals(path, fullRoot)) throw UnsafePath("The content manifest contains a path outside the staging directory.");
        return path;
    }

    internal static Dictionary<string, string> ResolveManifestPaths(string root, IEnumerable<DepotManifest.FileData> files)
    {
        var resolved = new Dictionary<string, string>(StringComparer.Ordinal);
        var explicitPaths = new HashSet<string>(PathComparer);
        var nodes = new Dictionary<string, (string Path, bool Directory)>(PathComparer);
        var fullRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));
        foreach (var file in files)
        {
            if (file.Flags.HasFlag(EDepotFileFlag.Symlink)) throw new ServiceException("UNSUPPORTED_SYMLINK", "The content manifest contains a symbolic link.");
            var path = ResolveContainedPath(fullRoot, file.FileName);
            if (!explicitPaths.Add(path)) throw UnsafePath("The content manifest contains duplicate or case-colliding paths.");
            resolved.Add(file.FileName, path);
            var directory = file.Flags.HasFlag(EDepotFileFlag.Directory);
            for (var current = path; !PathComparer.Equals(current, fullRoot); current = Path.GetDirectoryName(current)!)
            {
                if (nodes.TryGetValue(current, out var existing))
                {
                    if (existing.Directory != directory || !StringComparer.Ordinal.Equals(existing.Path, current))
                        throw UnsafePath("The content manifest contains conflicting file or directory paths.");
                }
                else nodes.Add(current, (current, directory));
                directory = true;
            }
        }
        return resolved;
    }

    internal static void CreateDirectory(string path)
    {
        ValidateAncestors(path);
        Directory.CreateDirectory(path);
        ValidateAncestors(path);
    }

    internal static void ValidateAncestors(string path)
    {
        for (string? current = Path.GetFullPath(path); current != null; current = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(current)))
        {
            try { ValidatePathIsNotLink(current); }
            catch (FileNotFoundException) { }
            catch (DirectoryNotFoundException) { }
        }
    }

    private static void CopyDirectory(string source, string destination, CancellationToken cancellationToken)
    {
        ValidateNoLinks(source, cancellationToken);
        CreateDirectory(destination);
        var pending = new Stack<(string Source, string Destination)>();
        pending.Push((source, destination));
        while (pending.TryPop(out var current))
        {
            cancellationToken.ThrowIfCancellationRequested();
            ValidateAncestors(current.Source);
            ValidateAncestors(current.Destination);
            foreach (var entry in Directory.EnumerateFileSystemEntries(current.Source))
            {
                cancellationToken.ThrowIfCancellationRequested();
                ValidatePathIsNotLink(entry);
                var target = Path.Combine(current.Destination, Path.GetFileName(entry));
                ValidateAncestors(target);
                if (Directory.Exists(entry))
                {
                    CreateDirectory(target);
                    pending.Push((entry, target));
                }
                else File.Copy(entry, target, true);
            }
        }
    }

    internal static void ValidateNoLinks(string root, CancellationToken cancellationToken)
    {
        ValidateAncestors(root);
        ValidatePathIsNotLink(root);
        var pending = new Stack<string>();
        pending.Push(root);
        while (pending.TryPop(out var directory))
        {
            cancellationToken.ThrowIfCancellationRequested();
            ValidateAncestors(directory);
            foreach (var entry in Directory.EnumerateFileSystemEntries(directory))
            {
                cancellationToken.ThrowIfCancellationRequested();
                ValidatePathIsNotLink(entry);
                if (Directory.Exists(entry)) pending.Push(entry);
            }
        }
    }

    internal static void ValidatePathIsNotLink(string path)
    {
        var attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.ReparsePoint) != 0) throw UnsafePath("The content directory contains a symbolic link or reparse point.");
        // A plain hard link is not a reparse point: writing the staging buffer
        // would also rewrite every other name that shares the inode.
        if (OperatingSystem.IsWindows() && (attributes & FileAttributes.Directory) == 0 && FileLinkCount(path) > 1)
            throw UnsafePath("The content directory contains a hard-linked file.");
    }

    private static uint FileLinkCount(string path)
    {
        using var handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        if (!GetFileInformationByHandle(handle, out var information)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return information.NumberOfLinks;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FileTime
    {
        public uint Low;
        public uint High;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ByHandleFileInformation
    {
        public uint FileAttributes;
        public FileTime CreationTime;
        public FileTime LastAccessTime;
        public FileTime LastWriteTime;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle handle, out ByHandleFileInformation information);

    private static void ValidateDirectoryIfPresent(string path, CancellationToken cancellationToken)
    {
        ValidateAncestors(path);
        if (Directory.Exists(path)) ValidateNoLinks(path, cancellationToken);
        else if (File.Exists(path)) throw UnsafePath("The installation path is not a directory.");
    }

    internal static void RemoveUnlistedFiles(string root, HashSet<string> allowed, CancellationToken cancellationToken)
    {
        ValidateNoLinks(root, cancellationToken);
        var pending = new Stack<string>();
        var directories = new List<string>();
        pending.Push(root);
        while (pending.TryPop(out var current))
        {
            cancellationToken.ThrowIfCancellationRequested();
            ValidateAncestors(current);
            foreach (var entry in Directory.EnumerateFileSystemEntries(current))
            {
                cancellationToken.ThrowIfCancellationRequested();
                ValidatePathIsNotLink(entry);
                if (Directory.Exists(entry))
                {
                    pending.Push(entry);
                    directories.Add(entry);
                }
                else if (!allowed.Contains(Path.GetFullPath(entry))) File.Delete(entry);
            }
        }
        foreach (var directory in directories.OrderByDescending(path => path.Length))
        {
            cancellationToken.ThrowIfCancellationRequested();
            ValidateAncestors(directory);
            if (!Directory.EnumerateFileSystemEntries(directory).Any()) Directory.Delete(directory);
        }
    }

    internal static void Install(string staging, string final, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        ValidateNoLinks(staging, cancellationToken);
        ValidateDirectoryIfPresent(final, cancellationToken);
        var backup = final + ".previous";
        ValidateDirectoryIfPresent(backup, cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();
        if (!Directory.Exists(final) && Directory.Exists(backup)) Directory.Move(backup, final);
        if (Directory.Exists(backup)) Directory.Delete(backup, true);
        cancellationToken.ThrowIfCancellationRequested();
        var movedPrevious = false;
        try
        {
            if (Directory.Exists(final))
            {
                Directory.Move(final, backup);
                movedPrevious = true;
            }
            cancellationToken.ThrowIfCancellationRequested();
            ValidateAncestors(staging);
            ValidateAncestors(final);
            Directory.Move(staging, final);
        }
        catch
        {
            if (movedPrevious && !Directory.Exists(final))
            {
                ValidateAncestors(backup);
                ValidateAncestors(final);
                Directory.Move(backup, final);
            }
            throw;
        }
        if (Directory.Exists(backup))
        {
            // 提交后清理失败时保留备份，不把成功安装误报成失败。
            try
            {
                ValidateNoLinks(backup, CancellationToken.None);
                Directory.Delete(backup, true);
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            catch (ServiceException) { }
        }
    }

    private static ServiceException UnsafePath(string message) => new("UNSAFE_PATH", message);
}
