using Translator.Core.Parsing.Dol;
using Translator.Core.Parsing.Rel;

namespace Translator.Core.Loading;

/// <summary>A flat, pre-linked binary image placed at a fixed guest base.</summary>
/// <remarks>Sin &amp; Punishment's stage modules (tools/rso_link.py) are linked to
/// a stable window (0x80800000) ahead of time, so module support needs no
/// relocation here: the bytes are copied verbatim into the RAM window.</remarks>
public sealed record FlatModuleImage(
    string Path,
    uint BaseAddress,
    byte[] Data)
{
    public AddressRange Range => new(BaseAddress, checked(BaseAddress + (uint)Data.Length));
}

public sealed class ProgramImage
{
    public ProgramImage(
        byte[] memory,
        AddressRange usedRange,
        AddressRange dolRange,
        AddressRange relRange,
        string sha256,
        uint memoryBase = MemoryLayout.RamBase)
        : this(memory, usedRange, dolRange, relRange, new AddressRange(0, 0), sha256, memoryBase)
    {
    }

    public ProgramImage(
        byte[] memory,
        AddressRange usedRange,
        AddressRange dolRange,
        AddressRange relRange,
        AddressRange moduleRange,
        string sha256,
        uint memoryBase = MemoryLayout.RamBase)
    {
        Memory = memory;
        UsedRange = usedRange;
        DolRange = dolRange;
        RelRange = relRange;
        ModuleRange = moduleRange;
        Sha256 = sha256;
        MemoryBase = memoryBase;
    }

    public byte[] Memory { get; }
    public uint MemoryBase { get; }
    public uint MemoryEnd => checked(MemoryBase + (uint)Memory.Length);
    public AddressRange UsedRange { get; }
    public AddressRange DolRange { get; }
    public AddressRange RelRange { get; }
    public AddressRange ModuleRange { get; }
    public bool HasRel => RelRange.End > RelRange.Start;
    public bool HasModule => ModuleRange.End > ModuleRange.Start;
    public string Sha256 { get; }

    public bool Contains(uint address, int size = 1)
    {
        if (size < 0 || address < MemoryBase)
        {
            return false;
        }

        var offset = (ulong)address - MemoryBase;
        return offset + (uint)size <= (ulong)Memory.Length;
    }

    public int GetOffset(uint address, int size = 1)
    {
        if (!Contains(address, size))
        {
            throw new ArgumentOutOfRangeException(
                nameof(address),
                $"Guest range 0x{address:X8}+0x{size:X} is outside 0x{MemoryBase:X8}-0x{MemoryEnd:X8}.");
        }

        return checked((int)(address - MemoryBase));
    }
}
