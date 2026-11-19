namespace Translator.Tests;

internal static class ProjectPathsForTests
{
    public static string FindRepositoryRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "translator", "Translator.sln")) &&
                File.Exists(Path.Combine(dir.FullName, "translator", "src", "Translator.Core", "Translator.Core.csproj")))
                return dir.FullName;
            dir = dir.Parent;
        }

        throw new DirectoryNotFoundException("Could not find the translator repository root.");
    }

    public static string FindDonorRepositoryRoot()
    {
        var path = Environment.GetEnvironmentVariable(IntegrationTestConfiguration.DonorRootVariable);
        if (string.IsNullOrWhiteSpace(path))
            throw new InvalidOperationException(IntegrationTestConfiguration.DonorSkipReason);
        path = Path.GetFullPath(path);
        if (!Directory.Exists(Path.Combine(path, "runtime", "src")) ||
            !File.Exists(Path.Combine(path, "projects", "mkwii", "recomp.yml")))
            throw new DirectoryNotFoundException(
                $"RECOMP_DONOR_ROOT is not a compatible Wiicompiled checkout: {path}");
        return path;
    }
}
