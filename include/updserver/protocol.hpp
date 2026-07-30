#pragma once

#include "cpp_compat.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <bit>
#include <array>

namespace updclient::updserver {

constexpr uint16_t ANNC_PORT = 48;
constexpr uint16_t NANDSVR_PORT = 49;
constexpr uint32_t CMD_MAGIC_BE = 0x4E537672; // 'NSvr' Big-Endian

enum class CommandOp : uint32_t {
    Quit = 0,
    Shutdown,
    Reboot,
    SmcReboot,
    GetInfo,
    GetBbList,
    GetFlash,
    WriteFlash,
    ReadBlock,
    WriteBlock,
    EraseBlock,
    GetBootloaders,
    WritePatch,
    MountPath,
    UnmountPath,
    GetFile,
    SendFile,
    FormatSysEx,
    FormatCompat,
    MkDir,
    HvPeek,
    HvPoke,
    Peek,
    Poke,
    Get1Bl,
    HvDump,
    GetVer,
    Max
};

#pragma pack(push, 1)
struct NandInfo {
    uint32_t structVer;   // Format version (peek ver in lower byte)
    uint32_t kernelVer;   // System kernel version
    uint32_t optFlag;     // Devkit / feature flags
    uint32_t useFlags;    // Console hardware type flags
    uint32_t hwFlags;     // Hardware info flags
    uint32_t dumpSize;    // Full NAND dump size in bytes
    uint32_t blockSize;   // Flash block size including spare
    uint32_t pairing;     // Pairing data
    uint8_t cpuKey[16];   // CPU Key
    uint8_t dvdKey[16];   // DVD Key
    uint8_t fuses[0x60];  // Fuses
    uint8_t vFuses[0x60]; // Virtual Fuses
    uint8_t bl1Key[16];   // 1BL RSA magic key
    uint8_t bl1Rsa[0x110];
    uint8_t pirsRsa[0x110];
    uint8_t masterRsa[0x110];
    uint8_t cbaNonce[0x10];
    uint8_t cbbNonce[0x10];
    uint8_t cdNonce[0x10];
    uint8_t ceNonce[0x10];
    uint8_t cfNonce[0x10];
    uint8_t cgNonce[0x10];
};

struct UdpBcastMsg {
    uint32_t magic;
    uint32_t ipAddr; // Big endian in_addr
};
#pragma pack(pop)

// Endianness Helpers
template <typename T>
requires std::is_integral_v<T>
constexpr T swap_be(T val) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
#if defined(_MSC_VER)
        if constexpr (sizeof(T) == 2) return _byteswap_ushort(static_cast<uint16_t>(val));
        if constexpr (sizeof(T) == 4) return _byteswap_ulong(static_cast<uint32_t>(val));
        if constexpr (sizeof(T) == 8) return _byteswap_uint64(static_cast<uint64_t>(val));
#else
        if constexpr (sizeof(T) == 2) return __builtin_bswap16(static_cast<uint16_t>(val));
        if constexpr (sizeof(T) == 4) return __builtin_bswap32(static_cast<uint32_t>(val));
        if constexpr (sizeof(T) == 8) return __builtin_bswap64(static_cast<uint64_t>(val));
#endif
        return val;
    } else {
        return val;
    }
}

inline NandInfo swap_nand_info(const NandInfo& in) noexcept {
    NandInfo out = in;
    out.structVer = swap_be(in.structVer);
    out.kernelVer = swap_be(in.kernelVer);
    out.optFlag = swap_be(in.optFlag);
    out.useFlags = swap_be(in.useFlags);
    out.hwFlags = swap_be(in.hwFlags);
    out.dumpSize = swap_be(in.dumpSize);
    out.blockSize = swap_be(in.blockSize);
    out.pairing = swap_be(in.pairing);
    return out;
}

} // namespace updclient::updserver

namespace updclient {
UPDCLIENT_API std::string format_hex_bytes(const uint8_t* data, size_t length);
}
