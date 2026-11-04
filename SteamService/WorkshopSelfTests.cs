using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;
using SteamKit2;
using SteamKit2.CDN;

namespace MirageSteamService;

internal static class WorkshopSelfTests
{
    internal static int Run()
    {
        var root = Path.Combine(Path.GetTempPath(), "MirageSteamService-tests-中文 空格-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        var tests = new (string Name, Action<string> Run)[]
        {
            ("路径边界与 Windows 文件名", TestPaths),
            ("清单重复、大小写和目录冲突", TestManifestPaths),
            ("平台比较器与未列出文件清理", TestUnlistedFiles),
            ("持久安装、替换与中断恢复", TestInstallation),
            ("取消与重命名失败保留旧安装", TestCancelledInstall),
            ("损坏内容与异常 chunk 范围", TestValidation),
            ("可执行权限跨平台", TestExecutableMode),
            ("符号链接及 junction 包含检测", TestLinks)
        };
        var failures = 0;
        try
        {
            foreach (var test in tests)
            {
                var testRoot = Path.Combine(root, Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(testRoot);
                try
                {
                    test.Run(testRoot);
                    Console.WriteLine($"PASS {test.Name}");
                }
                catch (Exception error)
                {
                    failures++;
                    Console.Error.WriteLine($"FAIL {test.Name}: {error}");
                }
            }
            Console.WriteLine($"离线自检：{tests.Length - failures}/{tests.Length} 通过。");
            return failures == 0 ? 0 : 1;
        }
        finally
        {
            DeleteTestTree(root);
        }
    }

    private static void TestPaths(string root)
    {
        var nested = WorkshopStorage.ResolveContainedPath(root + Path.DirectorySeparatorChar, "assets\\nested/file.bin");
        Assert(WorkshopStorage.PathComparer.Equals(nested, Path.Combine(root, "assets", "nested", "file.bin")), "合法混合分隔符路径解析失败。");
        foreach (var relative in new[] { "", " ", ".", "..", "../outside.bin", "..\\outside.bin", "../" + Path.GetFileName(root) + "-sibling/a", "a/../../escape", "/absolute", "\\rooted", "C:\\outside.bin", "C:relative.bin", "\\\\server\\share\\item", "\\\\?\\C:\\item", "file:stream", "folder//item", "folder/./item", "folder/../item", "null\0byte" })
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ResolveContainedPath(root, relative));
        if (OperatingSystem.IsWindows())
        {
            foreach (var relative in new[] { "CON", "con.txt", "CON .txt", "AUX.bin", "PRN", "NUL", "COM1.json", "COM1 .json", "LPT9", "COM¹.txt", "CONIN$", "CONOUT$", "folder./a", "folder /a", "trailing.", "trailing ", "a<b", "a>b", "a\"b", "a|b", "a?b", "a*b", "a\u0001b" })
                ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ResolveContainedPath(root, relative));
            Assert(WorkshopStorage.ResolveContainedPath(root, "COM10.txt").EndsWith("COM10.txt", StringComparison.Ordinal), "错误拒绝合法 Windows 文件名。");
        }
        var volumeRoot = Path.GetPathRoot(root)!;
        var volumeChild = WorkshopStorage.ResolveContainedPath(volumeRoot, "safe-child");
        Assert(WorkshopStorage.PathComparer.Equals(volumeChild, Path.Combine(volumeRoot, "safe-child")), "磁盘根目录边界解析失败。");
    }

    private static void TestManifestPaths(string root)
    {
        WorkshopStorage.ResolveManifestPaths(root, [File("project.json"), File("assets", EDepotFileFlag.Directory), File("assets/image.bin")]);
        ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ResolveManifestPaths(root, [File("same.bin"), File("same.bin")]));
        ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ResolveManifestPaths(root, [File("same/a.bin"), File("same\\a.bin")]));
        ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ResolveManifestPaths(root, [File("assets"), File("assets/image.bin")]));
        ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ResolveManifestPaths(root, [File("assets/image.bin"), File("assets")]));
        ExpectCode("UNSUPPORTED_SYMLINK", () => WorkshopStorage.ResolveManifestPaths(root, [File("link", EDepotFileFlag.Directory | EDepotFileFlag.Symlink)]));
        if (OperatingSystem.IsWindows())
        {
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ResolveManifestPaths(root, [File("PROJECT.JSON"), File("project.json")]));
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ResolveManifestPaths(root, [File("Assets/a.bin"), File("assets/b.bin")]));
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ResolveManifestPaths(root, [File("Assets/a.bin"), File("assets", EDepotFileFlag.Directory)]));
        }
        else
        {
            WorkshopStorage.ResolveManifestPaths(root, [File("upper.bin"), File("UPPER.bin")]);
        }
    }

    private static void TestUnlistedFiles(string root)
    {
        var listed = Path.Combine(root, "Project.JSON");
        System.IO.File.WriteAllText(listed, "{}");
        var allowedPath = OperatingSystem.IsWindows() ? Path.Combine(root, "project.json") : listed;
        Directory.CreateDirectory(Path.Combine(root, "obsolete"));
        System.IO.File.WriteAllText(Path.Combine(root, "obsolete", "unlisted.bin"), "old");
        WorkshopStorage.RemoveUnlistedFiles(root, new HashSet<string>(WorkshopStorage.PathComparer) { allowedPath }, CancellationToken.None);
        Assert(System.IO.File.Exists(listed), "清理误删清单中列出的文件。");
        Assert(!Directory.Exists(Path.Combine(root, "obsolete")), "清理未删除过期文件与空目录。");
    }

    private static void TestInstallation(string root)
    {
        var paths = WorkshopStorage.Prepare(root, 1, CancellationToken.None);
        System.IO.File.WriteAllText(Path.Combine(paths.Staging, "project.json"), "first");
        WorkshopStorage.Install(paths.Staging, paths.Final, CancellationToken.None);
        Assert(System.IO.File.ReadAllText(Path.Combine(paths.Final, "project.json")) == "first", "首次安装失败。");
        paths = WorkshopStorage.Prepare(root, 1, CancellationToken.None);
        Assert(System.IO.File.ReadAllText(Path.Combine(paths.Staging, "project.json")) == "first", "持久安装不能复用。");
        System.IO.File.WriteAllText(Path.Combine(paths.Staging, "project.json"), "second");
        Assert(System.IO.File.ReadAllText(Path.Combine(paths.Final, "project.json")) == "first", "staging 修改影响旧安装。");
        WorkshopStorage.Install(paths.Staging, paths.Final, CancellationToken.None);
        Assert(System.IO.File.ReadAllText(Path.Combine(paths.Final, "project.json")) == "second", "替换安装失败。");
        Assert(!Directory.Exists(paths.Final + ".previous"), "成功提交未清理备份。");
        Directory.Move(paths.Final, paths.Final + ".previous");
        paths = WorkshopStorage.Prepare(root, 1, CancellationToken.None);
        Assert(System.IO.File.ReadAllText(Path.Combine(paths.Final, "project.json")) == "second", "中断提交不能恢复备份。");
        Assert(System.IO.File.ReadAllText(Path.Combine(paths.Staging, "project.json")) == "second", "恢复后的 staging 不能复用。");
    }

    private static void TestCancelledInstall(string root)
    {
        var paths = WorkshopStorage.Prepare(root, 2, CancellationToken.None);
        Directory.CreateDirectory(paths.Final);
        System.IO.File.WriteAllText(Path.Combine(paths.Final, "project.json"), "old");
        System.IO.File.WriteAllText(Path.Combine(paths.Staging, "project.json"), "new");
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        Expect<OperationCanceledException>(() => WorkshopStorage.Install(paths.Staging, paths.Final, cancellation.Token));
        Assert(System.IO.File.ReadAllText(Path.Combine(paths.Final, "project.json")) == "old", "取消丢失旧安装。");
        Assert(System.IO.File.ReadAllText(Path.Combine(paths.Staging, "project.json")) == "new", "取消丢失可恢复 staging。");
        var occupied = Path.Combine(root, "occupied");
        System.IO.File.WriteAllText(occupied, "not a directory");
        ExpectCode("UNSAFE_PATH", () => WorkshopStorage.Install(paths.Staging, occupied, CancellationToken.None));
        Assert(System.IO.File.ReadAllText(occupied) == "not a directory", "失败安装覆盖非目录目标。");
        if (OperatingSystem.IsWindows())
        {
            using var locked = new FileStream(Path.Combine(paths.Staging, "project.json"), FileMode.Open, FileAccess.Read, FileShare.Read);
            Expect<IOException>(() => WorkshopStorage.Install(paths.Staging, paths.Final, CancellationToken.None));
            Assert(System.IO.File.ReadAllText(Path.Combine(paths.Final, "project.json")) == "old", "Windows 重命名失败未回滚旧安装。");
            Assert(!Directory.Exists(paths.Final + ".previous"), "回滚后旧安装仍停留在备份路径。");
        }
    }

    private static void TestValidation(string root)
    {
        var bytes = new byte[] { 1, 2, 3, 4 };
        var data = File("project.json");
        data.TotalSize = (ulong)bytes.Length;
        data.FileHash = SHA1.HashData(bytes);
        data.Chunks.Add(new DepotManifest.ChunkData
        {
            Offset = 0,
            UncompressedLength = (uint)bytes.Length,
            CompressedLength = (uint)bytes.Length,
            Checksum = DepotChunk.AdlerHash(bytes)
        });
        var paths = WorkshopStorage.Prepare(root, 3, CancellationToken.None);
        Directory.CreateDirectory(paths.Final);
        System.IO.File.WriteAllText(Path.Combine(paths.Final, "project.json"), "old");
        var path = Path.Combine(paths.Staging, data.FileName);
        System.IO.File.WriteAllBytes(path, bytes);
        WorkshopDownloader.ValidateChunkLayout(data);
        WorkshopDownloader.ValidateFile(data, path);
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        Expect<OperationCanceledException>(() => WorkshopDownloader.ValidateFile(data, path, cancelled.Token));
        Assert(System.IO.File.ReadAllText(Path.Combine(paths.Final, "project.json")) == "old", "取消校验影响旧安装。");
        System.IO.File.WriteAllBytes(path, [1, 2, 3, 5]);
        ExpectCode("VALIDATION_FAILED", () => WorkshopDownloader.ValidateFile(data, path));
        Assert(System.IO.File.ReadAllText(Path.Combine(paths.Final, "project.json")) == "old", "损坏 staging 影响旧安装。");
        System.IO.File.WriteAllBytes(path, bytes);
        data.FileHash = new byte[20];
        ExpectCode("VALIDATION_FAILED", () => WorkshopDownloader.ValidateFile(data, path));
        data.Chunks[0].Offset = ulong.MaxValue;
        ExpectCode("INVALID_MANIFEST", () => WorkshopDownloader.ValidateChunkLayout(data));
        data.Chunks[0].Offset = 0;
        data.Chunks[0].UncompressedLength = 3;
        ExpectCode("INVALID_MANIFEST", () => WorkshopDownloader.ValidateChunkLayout(data));
        data.Chunks[0].UncompressedLength = 5;
        ExpectCode("INVALID_MANIFEST", () => WorkshopDownloader.ValidateChunkLayout(data));
        data.Chunks.Clear();
        ExpectCode("INVALID_MANIFEST", () => WorkshopDownloader.ValidateChunkLayout(data));
        var empty = File("empty");
        WorkshopDownloader.ValidateChunkLayout(empty);
        System.IO.File.WriteAllBytes(Path.Combine(root, "empty"), []);
        WorkshopDownloader.ValidateFile(empty, Path.Combine(root, "empty"));
    }

    private static void TestExecutableMode(string root)
    {
        var path = Path.Combine(root, "executable.bin");
        System.IO.File.WriteAllText(path, "test");
        WorkshopDownloader.ApplyExecutableMode(File("executable.bin", EDepotFileFlag.Executable), path);
        if (!OperatingSystem.IsWindows())
        {
            var expected = UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute;
            Assert((System.IO.File.GetUnixFileMode(path) & expected) == expected, "Unix 可执行权限没有保留。");
        }
    }

    private static void TestLinks(string root)
    {
        var outside = Path.Combine(root, "outside");
        Directory.CreateDirectory(outside);
        var marker = Path.Combine(outside, "marker");
        System.IO.File.WriteAllText(marker, "untouched");
        var link = Path.Combine(root, "linked-root");
        CreateDirectoryLink(link, outside);
        try
        {
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.Prepare(link, 4, CancellationToken.None));
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.Prepare(Path.Combine(link, "nested"), 4, CancellationToken.None));
        }
        finally { Directory.Delete(link); }
        var output = Path.Combine(root, "output");
        Directory.CreateDirectory(output);
        var stagingParent = Path.Combine(output, ".staging");
        CreateDirectoryLink(stagingParent, outside);
        try { ExpectCode("UNSAFE_PATH", () => WorkshopStorage.Prepare(output, 4, CancellationToken.None)); }
        finally { Directory.Delete(stagingParent); }
        var finalLink = Path.Combine(output, "4");
        CreateDirectoryLink(finalLink, outside);
        try { ExpectCode("UNSAFE_PATH", () => WorkshopStorage.Prepare(output, 4, CancellationToken.None)); }
        finally { Directory.Delete(finalLink); }
        var backupLink = finalLink + ".previous";
        CreateDirectoryLink(backupLink, outside);
        try { ExpectCode("UNSAFE_PATH", () => WorkshopStorage.Prepare(output, 4, CancellationToken.None)); }
        finally { Directory.Delete(backupLink); }
        var paths = WorkshopStorage.Prepare(output, 4, CancellationToken.None);
        var childLink = Path.Combine(paths.Staging, "child");
        CreateDirectoryLink(childLink, outside);
        try
        {
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ValidateNoLinks(paths.Staging, CancellationToken.None));
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.RemoveUnlistedFiles(paths.Staging, new HashSet<string>(WorkshopStorage.PathComparer), CancellationToken.None));
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.Install(paths.Staging, paths.Final, CancellationToken.None));
        }
        finally { Directory.Delete(childLink); }
        Directory.CreateDirectory(paths.Final);
        var sourceLink = Path.Combine(paths.Final, "child");
        CreateDirectoryLink(sourceLink, outside);
        try { ExpectCode("UNSAFE_PATH", () => WorkshopStorage.Prepare(output, 4, CancellationToken.None)); }
        finally { Directory.Delete(sourceLink); }
        Assert(System.IO.File.ReadAllText(marker) == "untouched", "包含检测触及目录外文件。");
        if (!OperatingSystem.IsWindows()) return;
        var victim = Path.Combine(outside, "victim.txt");
        System.IO.File.WriteAllText(victim, "victim");
        var hardLink = Path.Combine(paths.Staging, "hardlinked.bin");
        if (!CreateHardLinkW(hardLink, victim, IntPtr.Zero)) return;
        try
        {
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ValidatePathIsNotLink(hardLink));
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.ValidateNoLinks(paths.Staging, CancellationToken.None));
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.RemoveUnlistedFiles(paths.Staging, new HashSet<string>(WorkshopStorage.PathComparer), CancellationToken.None));
            ExpectCode("UNSAFE_PATH", () => WorkshopStorage.Install(paths.Staging, paths.Final, CancellationToken.None));
        }
        finally { System.IO.File.Delete(hardLink); }
        Assert(System.IO.File.ReadAllText(victim) == "victim", "硬链接检测触及目录外文件。");
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLinkW(string link, string target, IntPtr securityAttributes);

    private static void CreateDirectoryLink(string link, string target)
    {
        if (!OperatingSystem.IsWindows())
        {
            Directory.CreateSymbolicLink(link, target);
            return;
        }
        // 直接设置 junction，不启动 shell，也不要求符号链接管理员权限。
        Directory.CreateDirectory(link);
        using var handle = CreateFileW(link, 0x40000000, 7, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
        if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
        var absolute = Path.GetFullPath(target);
        var substituteName = absolute.StartsWith("\\\\", StringComparison.Ordinal) ? @"\??\UNC\" + absolute[2..] : @"\??\" + absolute;
        var substitute = Encoding.Unicode.GetBytes(substituteName);
        var display = Encoding.Unicode.GetBytes(absolute);
        var buffer = new byte[16 + substitute.Length + 2 + display.Length + 2];
        using (var stream = new MemoryStream(buffer))
        using (var writer = new BinaryWriter(stream))
        {
            writer.Write(0xA0000003u);
            writer.Write(checked((ushort)(buffer.Length - 8)));
            writer.Write((ushort)0);
            writer.Write((ushort)0);
            writer.Write(checked((ushort)substitute.Length));
            writer.Write(checked((ushort)(substitute.Length + 2)));
            writer.Write(checked((ushort)display.Length));
            writer.Write(substitute);
            writer.Write((ushort)0);
            writer.Write(display);
            writer.Write((ushort)0);
        }
        if (!DeviceIoControl(handle, 0x000900A4, buffer, buffer.Length, IntPtr.Zero, 0, out _, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr securityAttributes, uint creationDisposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint controlCode, byte[] input, int inputLength, IntPtr output, int outputLength, out int bytesReturned, IntPtr overlapped);

    private static void DeleteTestTree(string root)
    {
        foreach (var entry in Directory.EnumerateFileSystemEntries(root))
        {
            var attributes = System.IO.File.GetAttributes(entry);
            if ((attributes & FileAttributes.Directory) != 0)
            {
                if ((attributes & FileAttributes.ReparsePoint) != 0) Directory.Delete(entry);
                else DeleteTestTree(entry);
            }
            else System.IO.File.Delete(entry);
        }
        Directory.Delete(root);
    }

    private static DepotManifest.FileData File(string name, EDepotFileFlag flags = 0) => new() { FileName = name, Flags = flags };

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static void ExpectCode(string code, Action action)
    {
        try { action(); }
        catch (ServiceException error) when (error.Code == code) { return; }
        throw new InvalidOperationException($"预期异常 {code} 未发生。");
    }

    private static void Expect<T>(Action action) where T : Exception
    {
        try { action(); }
        catch (T) { return; }
        throw new InvalidOperationException($"预期异常 {typeof(T).Name} 未发生。");
    }
}
