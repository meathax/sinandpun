using Xunit;

namespace Translator.Tests;

internal static class IntegrationTestConfiguration
{
    public const string DonorRootVariable = "RECOMP_DONOR_ROOT";
    public const string DonorSkipReason =
        "Requires a compatible Wiicompiled donor checkout in RECOMP_DONOR_ROOT; not Sin gameplay coverage.";

    public static bool HasDonorRoot =>
        !string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable(DonorRootVariable));
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class DonorIntegrationFactAttribute : FactAttribute
{
    public DonorIntegrationFactAttribute()
    {
        if (!IntegrationTestConfiguration.HasDonorRoot)
            Skip = IntegrationTestConfiguration.DonorSkipReason;
    }
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class DonorIntegrationTheoryAttribute : TheoryAttribute
{
    public DonorIntegrationTheoryAttribute()
    {
        if (!IntegrationTestConfiguration.HasDonorRoot)
            Skip = IntegrationTestConfiguration.DonorSkipReason;
    }
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ExternalDolFactAttribute : FactAttribute
{
    public ExternalDolFactAttribute()
    {
        if (string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("RECOMP_GENERIC_DOL")))
            Skip = "Set RECOMP_GENERIC_DOL to opt into external DOL parsing and single-entry translation.";
    }
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class LlvmOracleTheoryAttribute : TheoryAttribute
{
    public LlvmOracleTheoryAttribute()
    {
        if (Environment.GetEnvironmentVariable("RECOMP_LLVM_ORACLE") != "1")
            Skip = "Set RECOMP_LLVM_ORACLE=1 to run the external LLVM 18 disassembly oracle.";
    }
}
