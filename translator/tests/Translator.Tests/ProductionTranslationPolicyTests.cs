namespace Translator.Tests;

public sealed class ProductionTranslationPolicyTests
{
    [Fact]
    public void ProductionProjectIsStrict()
    {
        var repositoryRoot = ProjectPathsForTests.FindRepositoryRoot();
        var productionProject = File.ReadAllText(
            Path.Combine(repositoryRoot, "projects", "r2ve01", "recomp.yml"));
        Assert.Contains("game_id: R2VE01", productionProject, StringComparison.Ordinal);
        Assert.Contains("allow_unsupported_instructions: false", productionProject, StringComparison.Ordinal);
        Assert.DoesNotContain("allow_unsupported_instructions: true", productionProject, StringComparison.Ordinal);
    }
}
