using System.Security.Cryptography;
using Translator.Cli.Configuration;
using Xunit;

namespace Translator.Tests;

public sealed class ModuleProjectConfigTests
{
    [Fact]
    public void DolOnlyProjectLeavesModuleUnset()
    {
        using var fixture = new TempProject();
        var project = TranslationProjectConfig.Load(fixture.WriteProject());

        Assert.Null(project.Inputs.Module);
        Assert.Null(project.Inputs.Rel);
        Assert.Equal(fixture.DolPath, project.Inputs.Dol.Path);
    }

    [Fact]
    public void ModuleAndRelAreRetainedIndependentlyAndResolveFromWorkspaceRoot()
    {
        using var fixture = new TempProject();
        var project = TranslationProjectConfig.Load(
            fixture.WriteProject("path: modules/stage.bin\nload_address: 0x80500000", includeRel: true));

        var module = Assert.IsType<ProjectRelInput>(project.Inputs.Module);
        var rel = Assert.IsType<ProjectRelInput>(project.Inputs.Rel);
        Assert.Equal(fixture.Root, project.WorkspaceRoot);
        Assert.Equal(fixture.ModulePath, module.Path);
        Assert.Equal(0x80500000u, module.LoadAddress);
        Assert.Null(module.Sha256);
        Assert.Equal(fixture.RelPath, rel.Path);
        Assert.Equal(0x80600000u, rel.LoadAddress);
        Assert.Equal(fixture.DolPath, project.Inputs.Dol.Path);
    }

    [Fact]
    public void ModuleHashIsNormalizedAndVerified()
    {
        using var fixture = new TempProject();
        var hash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(fixture.ModulePath)));
        var manifest = fixture.WriteProject(
            $"path: modules/stage.bin\nload_address: 0x80500000\nsha256: '  {hash}  '");

        var project = TranslationProjectConfig.Load(manifest);
        var module = Assert.IsType<ProjectRelInput>(project.Inputs.Module);
        Assert.Equal(hash.ToLowerInvariant(), module.Sha256);

        // Re-loading the same pinned manifest must reject changed module bytes.
        File.WriteAllBytes(fixture.ModulePath, [0x60, 0x00, 0x00, 0x00]);
        var error = Assert.Throws<InvalidDataException>(() => TranslationProjectConfig.Load(manifest));
        Assert.Contains("Configured module SHA-256 does not match", error.Message, StringComparison.Ordinal);
        Assert.Contains(fixture.ModulePath, error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void MissingModuleIsRejectedEvenWithoutHash()
    {
        using var fixture = new TempProject();
        var manifest = fixture.WriteProject("path: modules/stage.bin\nload_address: 0x80500000");
        File.Delete(fixture.ModulePath);

        var error = Assert.Throws<FileNotFoundException>(() => TranslationProjectConfig.Load(manifest));

        Assert.Equal(fixture.ModulePath, error.FileName);
        Assert.Contains("Configured module input was not found", error.Message, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("0x80500000")]
    [InlineData("2152726528")]
    public void ModuleLoadAddressAcceptsHexAndDecimal(string address)
    {
        using var fixture = new TempProject();
        var project = TranslationProjectConfig.Load(
            fixture.WriteProject($"path: modules/stage.bin\nload_address: '{address}'"));

        Assert.Equal(0x80500000u, Assert.IsType<ProjectRelInput>(project.Inputs.Module).LoadAddress);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("-1")]
    [InlineData("0x100000000")]
    [InlineData("not-an-address")]
    public void MissingOrInvalidModuleLoadAddressIsRejected(string? address)
    {
        using var fixture = new TempProject();
        var fields = "path: modules/stage.bin";
        if (address is not null)
        {
            fields += $"\nload_address: '{address}'";
        }
        var manifest = fixture.WriteProject(fields);

        var error = Assert.Throws<InvalidDataException>(() => TranslationProjectConfig.Load(manifest));

        Assert.Contains("inputs.module.load_address", error.Message, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    public void DeclaredModuleRequiresNonblankPath(string? path)
    {
        using var fixture = new TempProject();
        var pathField = path is null ? string.Empty : $"path: '{path}'\n";
        var manifest = fixture.WriteProject(pathField + "load_address: 0x80500000");

        var error = Assert.Throws<InvalidDataException>(() => TranslationProjectConfig.Load(manifest));

        Assert.Contains("inputs.module.path", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void EmptyModuleObjectIsRejected()
    {
        using var fixture = new TempProject();
        var manifest = fixture.WriteProject(string.Empty);

        var error = Assert.Throws<InvalidDataException>(() => TranslationProjectConfig.Load(manifest));

        Assert.Contains("inputs.module.path", error.Message, StringComparison.Ordinal);
    }

    private sealed class TempProject : IDisposable
    {
        public TempProject()
        {
            Root = Path.Combine(Path.GetTempPath(), "module_recomp_project", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(Path.Combine(Root, "config"));
            Directory.CreateDirectory(Path.Combine(Root, "assets"));
            Directory.CreateDirectory(Path.Combine(Root, "modules"));
            DolPath = Path.Combine(Root, "assets", "main.dol");
            RelPath = Path.Combine(Root, "assets", "aux.rel");
            ModulePath = Path.Combine(Root, "modules", "stage.bin");
            // These tests exercise manifest validation, not DOL/REL decoding.
            File.WriteAllBytes(DolPath, new byte[0x100]);
            File.WriteAllBytes(RelPath, new byte[0x20]);
            File.WriteAllBytes(ModulePath, [0x4E, 0x80, 0x00, 0x20]);
        }

        public string Root { get; }
        public string DolPath { get; }
        public string RelPath { get; }
        public string ModulePath { get; }

        public string WriteProject(string? moduleFields = null, bool includeRel = false)
        {
            var manifest = """
                schema_version: 1
                workspace_root: ..
                project:
                  id: synthetic-module
                inputs:
                  dol:
                    path: assets/main.dol
                """ + "\n";
            if (includeRel)
            {
                manifest += "  rel:\n    path: assets/aux.rel\n    load_address: 0x80600000\n";
            }
            if (moduleFields is not null)
            {
                manifest += moduleFields.Length == 0
                    ? "  module: {}\n"
                    : "  module:\n" + string.Join("\n", moduleFields.Split('\n').Select(line => "    " + line)) + "\n";
            }
            var path = Path.Combine(Root, "config", "recomp.yml");
            File.WriteAllText(path, manifest);
            return path;
        }

        public void Dispose() => Directory.Delete(Root, recursive: true);
    }
}
