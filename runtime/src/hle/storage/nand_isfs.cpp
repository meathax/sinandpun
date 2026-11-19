// NAND/ISFS HLE: the IOS_* device layer.
//
// Shared state and helpers live in nand_internal.h.

#include "nand_internal.h"

#include "runtime_log.h"

struct ISFSFileStats {
    uint32_t length;    // File size in bytes
    uint32_t position;  // Current file position
};

// ============================================================================
// Device identifiers and ioctl commands
// ============================================================================

// Special FD for /dev/fs (the ISFS device)
static constexpr int32_t ISFS_DEV_FD = 1;
static constexpr int32_t ES_DEV_FD = 3;
static constexpr uint32_t ES_IOCTL_GETTITLEID = 0x20;

static std::string ReadGuestCString(uint32_t address, size_t maxLength = 1024) {
    std::string text;
    if (address == 0) {
        return text;
    }

    for (size_t i = 0; i < maxLength; ++i) {
        const uint32_t current = address + static_cast<uint32_t>(i);
        if (!Memory::Contains(current, 1)) {
            break;
        }
        const char ch = static_cast<char>(Memory::Read8(current));
        if (ch == '\0') {
            break;
        }
        text.push_back(ch);
    }
    return text;
}

// Same guest layout as the /dev/net ioctlv descriptors; see runtime_parse_helpers.h.
using IosVector = RuntimeHle::IoVector;
using RuntimeHle::ReadIoVector;

static IosVector ReadIosVector(uint32_t vectorPtr, uint32_t index) {
    return ReadIoVector(vectorPtr, index);
}

static uint64_t CurrentTitleId() {
    uint32_t low = CurrentTitleIdLo();
    return (static_cast<uint64_t>(kNandTitleIdHi) << 32) | low;
}

static bool WriteGuestBytes(uint32_t address, uint32_t size, const uint8_t* data, size_t dataSize) {
    if (address == 0 || size < dataSize || !Memory::Contains(address, dataSize)) {
        return false;
    }
    uint8_t* out = Memory::GetPointer(address, dataSize);
    std::memcpy(out, data, dataSize);
    return true;
}

static bool IsValidGuestRange(uint32_t address, uint32_t size) {
    return size == 0 || (address != 0 && Memory::Contains(address, size));
}

extern "C" int32_t NAND_IOS_Open_HLE(uint32_t pathPtr, uint32_t mode) {
    const std::string pathStorage = ReadGuestCString(pathPtr);
    const char* path = pathPtr == 0 ? nullptr : pathStorage.c_str();
    
    if (!path) {
        LogNandError("IOS_Open", "null path");
        return ISFS_EINVAL;
    }
    
    // Handle special device paths
    if (std::strncmp(path, "/dev/", 5) == 0) {
        if (std::strcmp(path, "/dev/fs") == 0) {
            return ISFS_DEV_FD;
        }
        if (std::strcmp(path, "/dev/es") == 0) {
            return ES_DEV_FD;
        }
        if (const int32_t netFd = Network_HLE_OpenDevice(path, mode)) {
            return netFd;
        }
        if (const int32_t btFd = Bluetooth_HLE_OpenDevice(path)) {
            return btFd;
        }
        LogNandWarning("IOS_Open", "unknown device '%s' mode=%u", path, mode);
        return ISFS_ENOENT;
    }
    
    // It's a NAND file path
    const std::filesystem::path hostPath = TranslateNandPath(path);
    if (hostPath.empty()) {
        LogNandError("IOS_Open", "FAILED to translate path '%s'", path);
        return ISFS_EINVAL;
    }

    if (const auto result = NandCheckSystemSaveRead("IOS_Open", hostPath, mode, true))
        return *result;
    
    // Seed FaceLib resources before the existence check so every open mode can
    // still find them on a fresh managed NAND.
    if (!PathExists(hostPath) && IsFaceLibResourcePath(path)) {
        SeedFaceLibResource(hostPath);
    }

    // Determine file mode. IOS never creates files on open - creation happens
    // exclusively through ISFS CreateFile (which we implement). The previous
    // create-on-open fallback ("w+b") silently materialized 0-byte files (for
    // example /shared2/sys/net/02/config.dat) that later reads treated as
    // valid, poisoning persistent state across sessions.
    const char* fopenMode = "rb";
    if (mode == 2 || mode == 3) {
        if (!PathExists(hostPath)) {
            LogNandWarning("IOS_Open", "'%s' does not exist; open mode %u never creates it",
                    HostPathText(hostPath).c_str(), mode);
            return ISFS_ENOENT;
        }
        fopenMode = "r+b";      // Write-only opens still need read for seeks
    }

    FILE* file = NandFopen(hostPath, fopenMode);

    if (!file) {
        LogNandError("IOS_Open", "FAILED to open '%s'", HostPathText(hostPath).c_str());
        return ISFS_ENOENT;
    }
    
    int32_t fd = AllocateFd(hostPath, file, mode);
    return fd;
}
PPC_NATIVE_OVERRIDE(80199D00, NAND_IOS_Open_HLE, int32_t, (uint32_t pathPtr, uint32_t mode), (pathPtr, mode));

extern "C" int32_t NAND_IOS_Close_HLE(uint32_t fd) {
    if (fd == ISFS_DEV_FD) {
        return ISFS_OK;
    }
    if (fd == ES_DEV_FD) {
        return ISFS_OK;
    }
    if (Network_HLE_IsFd(fd)) {
        return Network_HLE_Close(fd);
    }
    if (Bluetooth_HLE_IsFd(fd)) {
        return Bluetooth_HLE_Close(fd);
    }
    
    auto* handle = GetHandle(fd);
    if (!handle) {
        LogNandError("IOS_Close", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    CloseFd(fd);
    return ISFS_OK;
}
PPC_NATIVE_OVERRIDE(80199EF0, NAND_IOS_Close_HLE, int32_t, (uint32_t fd), (fd));

extern "C" int32_t NAND_IOS_Read_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Read", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (!bufferPtr || length == 0) {
        return 0;
    }
    
    // The host pointer is only valid for the range it was resolved with.
    if (!Memory::Contains(bufferPtr, length)) {
        LogNandError("IOS_Read", "invalid buffer 0x%08X length=%u", bufferPtr, length);
        return ISFS_EINVAL;
    }
    uint8_t* buffer = Memory::GetPointer(bufferPtr, length);
    
    size_t bytesRead = std::fread(buffer, 1, length, handle->file);
    handle->position += static_cast<uint32_t>(bytesRead);
    
    return static_cast<int32_t>(bytesRead);
}
PPC_NATIVE_OVERRIDE(8019A0A0, NAND_IOS_Read_HLE, int32_t, (uint32_t fd, uint32_t bufferPtr, uint32_t length), (fd, bufferPtr, length));

extern "C" int32_t NAND_IOS_Write_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Write", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (!bufferPtr || length == 0) {
        return 0;
    }
    
    if (!Memory::Contains(bufferPtr, length)) {
        LogNandError("IOS_Write", "invalid buffer 0x%08X length=%u", bufferPtr, length);
        return ISFS_EINVAL;
    }
    const uint8_t* buffer = Memory::GetPointer(bufferPtr, length);
    
    size_t bytesWritten = std::fwrite(buffer, 1, length, handle->file);
    std::fflush(handle->file);
    handle->position += static_cast<uint32_t>(bytesWritten);
    
    return static_cast<int32_t>(bytesWritten);
}
PPC_NATIVE_OVERRIDE(8019A2B0, NAND_IOS_Write_HLE, int32_t, (uint32_t fd, uint32_t bufferPtr, uint32_t length), (fd, bufferPtr, length));

extern "C" int32_t NAND_IOS_Seek_HLE(uint32_t fd, int32_t offset, int32_t whence) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Seek", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (std::fseek(handle->file, offset, NandSeekOrigin(whence)) != 0) {
        LogNandError("IOS_Seek", "fd=%d offset=%d whence=%d FAILED", fd, offset, whence);
        return ISFS_EIO;
    }
    
    handle->position = static_cast<uint32_t>(std::ftell(handle->file));
    return static_cast<int32_t>(handle->position);
}
SIN_DISABLED_NATIVE_OVERRIDE(80194070, NAND_IOS_Seek_HLE, int32_t, (uint32_t fd, int32_t offset, int32_t whence), (fd, offset, whence));

// ============================================================================
// IOS_Ioctl HLE - Handles filesystem commands
// ============================================================================

// ISFS Ioctl commands
enum ISFSCommand {
    ISFS_IOCTL_FORMAT = 1,
    ISFS_IOCTL_GETSTATS = 2,
    ISFS_IOCTL_CREATEDIR = 3,
    ISFS_IOCTL_READDIR = 4,
    ISFS_IOCTL_SETATTR = 5,
    ISFS_IOCTL_GETATTR = 6,
    ISFS_IOCTL_DELETE = 7,
    ISFS_IOCTL_RENAME = 8,
    ISFS_IOCTL_CREATEFILE = 9,
    ISFS_IOCTL_SETFILEVERCTRL = 10,
    ISFS_IOCTL_GETFILESTATS = 11,
    ISFS_IOCTL_GETUSAGE = 12,
    ISFS_IOCTL_SHUTDOWN = 13,
};

extern "C" int32_t NAND_IOS_Ioctl_HLE(
    uint32_t fd,
    uint32_t cmd,
    uint32_t inBufPtr, uint32_t inLen,
    uint32_t outBufPtr, uint32_t outLen)
{

    if (Network_HLE_IsFd(fd)) {
        return Network_HLE_Ioctl(fd, cmd, inBufPtr, inLen, outBufPtr, outLen);
    }

    if (Bluetooth_HLE_IsFd(fd)) {
        LogNandWarning("IOS_Ioctl", "/dev/usb/oh1 does not support scalar ioctl cmd=%u", cmd);
        return -4;
    }
    
    // Handle /dev/fs ISFS commands
    if (fd == ISFS_DEV_FD) {
        switch (cmd) {
            case ISFS_IOCTL_CREATEDIR: {
                // Input buffer: path + attributes
                if (!inBufPtr || inLen < 0x4c) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr + 6);
                const std::filesystem::path hostPath = TranslateNandPath(path);
                if (hostPath.empty()) {
                    return ISFS_EINVAL;
                }

                if (CreateDirectoryPath(hostPath)) {
                    return ISFS_OK;
                }
                return ISFS_EIO;
            }
            
            case ISFS_IOCTL_DELETE: {
                if (!inBufPtr || inLen < 0x40) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr);
                const std::filesystem::path hostPath = TranslateNandPath(path);
                
                // fs::remove refuses a non-empty directory, matching rmdir.
                if (NandRemove(hostPath)) {
                    return ISFS_OK;
                }
                return ISFS_ENOENT;
            }
            
            case ISFS_IOCTL_GETATTR: {
                if (!inBufPtr || !outBufPtr) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr);
                const std::filesystem::path hostPath = TranslateNandPath(path);
                
                if (!PathExists(hostPath)) {
                    return ISFS_ENOENT;
                }
                
                // Return fake attributes (owner UID, group ID, permissions)
                // Format: u32 ownerID, u16 groupID, u8 ownerPerm, u8 groupPerm, u8 otherPerm, u8 attrs
                if (outLen >= 0x4c) {
                    if (!Memory::Contains(outBufPtr, outLen)) {
                        return ISFS_EINVAL;
                    }
                    std::memset(Memory::GetPointer(outBufPtr, outLen), 0, outLen);
                    // Owner UID = 0
                    Memory::Write32(outBufPtr, 0);
                    // Group ID = 0
                    Memory::Write16(outBufPtr + 4, 0);
                    // Permissions: 3 = read/write for all
                    Memory::Write8(outBufPtr + 0x49, 3); // owner perm
                    Memory::Write8(outBufPtr + 0x46, 3); // group perm
                    Memory::Write8(outBufPtr + 0x47, 3); // other perm
                    Memory::Write8(outBufPtr + 0x48, IsDirectory(hostPath) ? 2 : 1); // attrs (2=dir, 1=file)
                }
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_CREATEFILE: {
                if (!inBufPtr || inLen < 0x4c) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr + 6);
                const std::filesystem::path hostPath = TranslateNandPath(path);
                // IOS answers FS_EEXIST (-105) for an existing entry and never truncates it.
                // The Sin NAND library is the real guest code, so NANDCreate/NANDPrivateCreate
                // reach this ioctl directly; truncating here wiped existing save/state files
                // whenever the game used "create, ignore EEXIST, then open".
                if (!hostPath.empty() && PathExists(hostPath)) {
                    return ISFS_EEXIST;
                }
                CreateParentDirectories(hostPath);

                // Create empty file
                FILE* f = NandFopen(hostPath, "wb");
                if (f) {
                    std::fclose(f);
                    return ISFS_OK;
                }
                return ISFS_EIO;
            }
            
            case ISFS_IOCTL_GETFILESTATS: {
                // GETFILESTATS is addressed to a file fd, never to /dev/fs.
                LogNandWarning("IOS_Ioctl", "GETFILESTATS on ISFS device - unexpected");
                return ISFS_EINVAL;
            }
            
            case ISFS_IOCTL_RENAME: {
                if (!inBufPtr || inLen < 0x80) {
                    return ISFS_EINVAL;
                }
                const char* srcPath = (const char*)Memory::GetPointer(inBufPtr);
                const char* dstPath = (const char*)Memory::GetPointer(inBufPtr + 0x40);
                const std::filesystem::path srcHost = TranslateNandPath(srcPath);
                const std::filesystem::path dstHost = TranslateNandPath(dstPath);
                
                if (NandRename(srcHost, dstHost)) {
                    return ISFS_OK;
                }
                return ISFS_EIO;
            }
            
            case ISFS_IOCTL_GETSTATS: {
                // Return filesystem stats (fake values)
                if (outBufPtr && outLen >= 0x1c) {
                    Memory::Write32(outBufPtr + 0x00, 0x200000);  // Total blocks
                    Memory::Write32(outBufPtr + 0x04, 0x100000);  // Free blocks
                    Memory::Write32(outBufPtr + 0x08, 0);         // Used blocks
                    Memory::Write32(outBufPtr + 0x0C, 0);         // Bad blocks
                    Memory::Write32(outBufPtr + 0x10, 0);         // Reserved blocks
                    Memory::Write32(outBufPtr + 0x14, 0x20);      // Block size
                    Memory::Write32(outBufPtr + 0x18, 0);         // Free inodes
                }
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_SETATTR: {
                // Ignore attribute changes - we don't implement file permissions
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_GETUSAGE: {
                // Return usage info (fake values)
                if (outBufPtr && outLen >= 8) {
                    Memory::Write32(outBufPtr + 0, 100);   // Files
                    Memory::Write32(outBufPtr + 4, 10000); // Blocks used
                }
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_READDIR: {
                // Read directory listing
                // This is complex - return empty for now
                if (outBufPtr && outLen >= 4) {
                    Memory::Write32(outBufPtr, 0); // 0 entries
                }
                return ISFS_OK;
            }
            
            default:
                LogNandWarning("IOS_Ioctl", "unknown ISFS cmd=%u", cmd);
                return ISFS_OK;
        }
    }
    
    // Handle file-specific commands
    auto* handle = GetHandle(fd);
    if (handle && handle->file) {
        if (cmd == ISFS_IOCTL_GETFILESTATS) {
            // Get file stats
            if (!outBufPtr || outLen < 8) {
                return ISFS_EINVAL;
            }
            
            const NandFileExtent extent = NandProbeFileExtent(handle->file);
            Memory::Write32(outBufPtr, static_cast<uint32_t>(extent.size));
            Memory::Write32(outBufPtr + 4, static_cast<uint32_t>(extent.position));
            
            return ISFS_OK;
        }
    }
    
    // Unknown command - return success to not block game
    return ISFS_OK;
}

extern "C" void NAND_IOS_Ioctl_Entry_HLE(CpuContext* ctx) {
    const uint32_t fd = ctx->gpr[3];
    const uint32_t cmd = ctx->gpr[4];
    const uint32_t inBufPtr = ctx->gpr[5];
    const uint32_t inLen = ctx->gpr[6];
    const uint32_t outBufPtr = ctx->gpr[7];
    const uint32_t outLen = ctx->gpr[8];

    ctx->gpr[3] = static_cast<uint32_t>(
        NAND_IOS_Ioctl_HLE(fd, cmd, inBufPtr, inLen, outBufPtr, outLen));
}
PPC_NATIVE_OVERRIDE_VOID(8019A5E0, NAND_IOS_Ioctl_Entry_HLE, (CpuContext* ctx), (ctx));

// ============================================================================
// IOS_Ioctlv HLE - Vector Ioctl for complex ISFS operations
// ============================================================================

static int32_t HandleIsfsReadDir(uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    const bool countOnly = (numIn == 1 && numOut == 1);
    if (!countOnly && !(numIn == 2 && numOut == 2)) {
        LogNandWarning("IOS_Ioctlv", "READDIR unsupported vector shape numIn=%u numOut=%u",
                numIn, numOut);
        return ISFS_EINVAL;
    }

    const IosVector pathVec = ReadIosVector(vectorPtr, 0);
    const std::string wiiPath = ReadGuestCString(pathVec.address, 64);
    if (wiiPath.empty()) {
        return ISFS_EINVAL;
    }
    const std::filesystem::path hostPath = TranslateNandPath(wiiPath.c_str());
    if (!IsDirectory(hostPath)) {
        return PathExists(hostPath) ? ISFS_EINVAL : ISFS_ENOENT;
    }

    // NAND names are at most 12 characters; longer host names cannot exist on
    // a real NAND (this also hides *.nandsafe.tmp write shadows).
    constexpr size_t kMaxNandNameLength = 12;
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(hostPath, ec)) {
        std::string name = HostPathText(entry.path().filename());
        if (name.empty() || name.size() > kMaxNandNameLength) {
            continue;
        }
        names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());

    if (countOnly) {
        const IosVector countOut = ReadIosVector(vectorPtr, 1);
        if (countOut.size < 4 || !Memory::Contains(countOut.address, 4)) {
            return ISFS_EINVAL;
        }
        Memory::Write32(countOut.address, static_cast<uint32_t>(names.size()));
        return ISFS_OK;
    }

    const IosVector maxVec = ReadIosVector(vectorPtr, 1);
    const IosVector namesOut = ReadIosVector(vectorPtr, 2);
    const IosVector countOut = ReadIosVector(vectorPtr, 3);
    if (maxVec.size < 4 || !Memory::Contains(maxVec.address, 4) ||
        countOut.size < 4 || !Memory::Contains(countOut.address, 4) ||
        !IsValidGuestRange(namesOut.address, namesOut.size)) {
        return ISFS_EINVAL;
    }
    const uint32_t maxCount = Memory::Read32(maxVec.address);

    constexpr uint32_t kEntryWindow = 13; // 12 chars + terminator
    uint32_t cursor = 0;
    uint32_t written = 0;
    for (const std::string& name : names) {
        if (written >= maxCount || cursor + kEntryWindow > namesOut.size) {
            break;
        }
        uint8_t* out = Memory::GetPointer(namesOut.address + cursor, kEntryWindow);
        std::memset(out, 0, kEntryWindow);
        std::memcpy(out, name.data(), name.size());
        cursor += static_cast<uint32_t>(name.size()) + 1;
        ++written;
    }
    Memory::Write32(countOut.address, written);
    return ISFS_OK;
}

extern "C" int32_t NAND_IOS_Ioctlv_HLE(
    uint32_t fd,
    uint32_t cmd,
    uint32_t numIn,
    uint32_t numOut,
    uint32_t vectorPtr)
{

    if (Network_HLE_IsFd(fd)) {
        return Network_HLE_Ioctlv(fd, cmd, numIn, numOut, vectorPtr);
    }
    if (Bluetooth_HLE_IsFd(fd)) {
        return Bluetooth_HLE_Ioctlv(fd, cmd, numIn, numOut, vectorPtr);
    }

    if (fd == ISFS_DEV_FD) {
        if (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
            return ISFS_EINVAL;
        }
        if (cmd == ISFS_IOCTL_READDIR) {
            return HandleIsfsReadDir(numIn, numOut, vectorPtr);
        }
        return ISFS_OK;
    }

    if (fd == ES_DEV_FD) {
        if (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
            return ISFS_EINVAL;
        }

        switch (cmd) {
            case ES_IOCTL_GETTITLEID: {
                if (numIn != 0 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector out = ReadIosVector(vectorPtr, 0);
                if (out.size < 8 || out.address == 0 || !Memory::Contains(out.address, 8)) {
                    return ISFS_EINVAL;
                }
                const uint64_t titleId = CurrentTitleId();
                Memory::Write32(out.address, static_cast<uint32_t>(titleId >> 32));
                Memory::Write32(out.address + 4u, static_cast<uint32_t>(titleId));
                return ISFS_OK;
            }

            default:
                LogNandWarning("IOS_Ioctlv", "unsupported /dev/es cmd=%u", cmd);
                return ISFS_EINVAL;
        }
    }
    
    // Non-device ioctlv has no ISFS command we need to service.
    return ISFS_OK;
}

extern "C" void NAND_IOS_Ioctlv_Entry_HLE(CpuContext* ctx) {
    const uint32_t fd = ctx->gpr[3];
    const uint32_t cmd = ctx->gpr[4];
    const uint32_t numIn = ctx->gpr[5];
    const uint32_t numOut = ctx->gpr[6];
    const uint32_t vectorPtr = ctx->gpr[7];

    ctx->gpr[3] = static_cast<uint32_t>(
        NAND_IOS_Ioctlv_HLE(fd, cmd, numIn, numOut, vectorPtr));
}
SIN_DISABLED_NATIVE_OVERRIDE_VOID(801945E0, NAND_IOS_Ioctlv_Entry_HLE, (CpuContext* ctx), (ctx));
