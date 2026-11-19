// Sin & Punishment: Star Successor is a single-region project (R2VE01, NTSC-U).
// The runtime's MKW_GADDR(...) arguments are the game's own addresses, so every
// "identity" row maps each address to itself for the other region slots. The
// donor's MKWii range tables are gone with the MKWii identities themselves.
// An address outside the guest space has no known counterpart and fails to
// compile, which is exactly what we want to keep retargeted constants honest.

MKW_REGION_RANGE(NtscU, 80000000, 817FFFFF, +0x0)
MKW_REGION_RANGE(NtscJ, 80000000, 817FFFFF, +0x0)
