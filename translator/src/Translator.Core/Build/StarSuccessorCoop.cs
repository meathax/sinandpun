namespace Translator.Core.Build;

// R2VE01 guest operations whose effective operand depends on the local player.
// Fail generation if the translation no longer has the audited instruction shape.
internal static class StarSuccessorCoop
{
    public static string Lower(uint address, string source)
    {
        string Replace(string from, string to, int count = 1)
        {
            if (source.Split(from, StringSplitOptions.None).Length != count + 1)
                throw new InvalidDataException($"Unexpected co-op instruction at {address:X8}: {from}");
            return source.Replace(from, to, StringComparison.Ordinal);
        }
        switch (address)
        {
            case 0x80076D78: // DefaultPlayer constructor passes base HP to ObjHit::setLife.
                source = Replace("InvokeDirectCpu<0x800A42C0u>(ctx);",
                    "ctx->fpr[1].d = SinCoopBaseLife(ctx->fpr[1].d);\n    InvokeDirectCpu<0x800A42C0u>(ctx);");
                break;
            case 0x800F8014: // GAMEOVER may open without a preloaded jingle (slot == -1).
                // Advance the scene's visual timer normally. Never index the audio table with -1.
                source = Replace("InvokeDirectCpu<0x8003B950u>(ctx);",
                    "if (MemoryInline::FlatRead32(r3) == 0xFFFFFFFFu) ctx->gpr[3] = 1; else InvokeDirectCpu<0x8003B950u>(ctx);");
                source = Replace("InvokeDirectCpu<0x8003BB00u>(ctx);",
                    "if (MemoryInline::FlatRead32(r3) != 0xFFFFFFFFu) InvokeDirectCpu<0x8003BB00u>(ctx);");
                source = Replace("InvokeDirectCpu<0x8003BCC4u>(ctx);",
                    "if (MemoryInline::FlatRead32(r3) != 0xFFFFFFFFu) InvokeDirectCpu<0x8003BCC4u>(ctx);", 2);
                break;
            case 0x8002D208: // Both guest input polling passes use the enabled-seat mask.
                source = Replace("MemoryInline::FlatRead8((r15 + 6))", "SinCoopPollMask(MemoryInline::FlatRead8((r15 + 6)))", 2);
                break;
            case 0x8007651C: // Default stage factory, optional autostage fixture
                // Each playable body owns a sight. Skip the separate retail assist gun.
                source = Replace("loc_800765AC:", "loc_800765AC:\n    if (SinCoopUsesActorSights()) goto loc_800765C4;");
                source = Replace("goto loc_8007651C;", "SinCoopSeedTestCharacter(ctx);\n    goto loc_8007651C;");
                break;
            case 0x8006F4A8: // Player::end
                source = Replace("goto loc_8006F4A8;", "SinCoopEndActor(r3);\n    goto loc_8006F4A8;");
                break;
            case 0x8007345C: // Player::update, normal and reserve changeover branches
                source = Replace("r0 = (r0 & 128);", "r0 = SinCoopChange(r13, r3, r31, r0 & 128);", 2);
                break;
            case 0x80077FC4: // Ground movement uses the same pad passed by cbMdMove.
                source = Replace("MemoryInline::FlatReadFloat32((r4 + 12))", "SinCoopMoveAxis(r13, r29, r30, 12)");
                source = Replace("MemoryInline::FlatReadFloat32((r30 + 16))", "SinCoopMoveAxis(r13, r29, r30, 16)");
                break;
            case 0x80078C80: // DefaultPlayer::cbMdMove
                source = Replace("MemoryInline::FlatReadFloat32((r31 + 12))", "SinCoopMoveAxis(r13, r30, r31, 12)", 2);
                source = Replace("MemoryInline::FlatReadFloat32((r31 + 16))", "SinCoopMoveAxis(r13, r30, r31, 16)");
                source = Replace("r31 = (r5 + 1924);", "r31 = SinCoopPad(r13, r30, r5 + 1924);");
                break;
            case 0x80068930: // sight input, owner in sight+1320
                source = Replace("r3 = MemoryInline::FlatRead8((r3 + 9));", "r3 = SinCoopSightCharacter(r13, MemoryInline::FlatRead32(r31 + 1320), MemoryInline::FlatRead8(r3 + 9));", 3);
                source = Replace("r30 = (r4 + 1924);", "r30 = SinCoopPad(r13, MemoryInline::FlatRead32(r31 + 1320), r4 + 1924);");
                source = Replace("r4 = (r4 + 1924);", "r4 = SinCoopPad(r13, MemoryInline::FlatRead32(r31 + 1320), r4 + 1924);", 2);
                break;
            case 0x800C65E0: // Decode the second physical seat with the normal action decoder.
                source = Replace("loc_800C6660:", "loc_800C6660:\n    if (SinCoopDecodePad(ctx, r1, r28, r4)) goto loc_800C66E8;");
                break;
            default:
                return source;
        }
        return "#include \"sin_coop.h\"\n" + source;
    }
}
