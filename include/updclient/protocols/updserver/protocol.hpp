#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Wire-level definitions for UpdServer: constants, packed structures and
// endianness helpers. No I/O and no policy live here.
//
// All multi-byte integers on the wire are big-endian. Commands are either a
// 4-byte big-endian CommandOp, or an ASCII line "<WORD> <args>\n" where numeric
// arguments are uppercase hex.

namespace updclient::updserver {

constexpr uint16_t ANNC_PORT = 48;
constexpr uint16_t NANDSVR_PORT = 49;
constexpr uint32_t CMD_MAGIC_BE = 0x4E537672; // 'NSvr'

// Fixed-size replies.
constexpr size_t k1blBytes = 0x8000;

// Transfer chunking used for file transfers and flash dumps.
constexpr size_t kFileChunkBytes = 1452;
constexpr size_t kDumpChunkBytes = 64u * 1024u;

// Ceiling of what the wire format can express: file and payload sizes are uint32.
constexpr uint64_t kWireMaxSize = 0xFFFFFFFFull;

// Default caps for values supplied by the peer or by the caller. Every one of them
// can be changed per client through ClientLimits (see client.hpp). They exist so a
// hostile or broken server cannot make the client allocate or write without bound.
constexpr size_t kMaxBadBlockCount = 0x10000;                // block numbers are 16 bit
constexpr size_t kMaxBootloaderBytes = 16u * 1024u * 1024u;  // reply of GetBootloaders
constexpr size_t kMaxBlockPayload = 64u * 1024u * 1024u;     // reply of ReadBlock
constexpr size_t kMaxBlockWriteBytes = 1u * 1024u * 1024u;   // payload of WriteBlock
constexpr uint32_t kMaxBlockCount = 0x1000;                  // blocks per read/erase request
constexpr size_t kMaxPeekBytes = 16u * 1024u * 1024u;        // Peek / HvPeek length
constexpr uint64_t kMaxFileBytes = kWireMaxSize;             // streamed to disk, never buffered
constexpr uint64_t kMaxDumpBytes = kWireMaxSize;             // streamed to disk, never buffered
constexpr size_t kMaxCommandLineBytes = 1024;                // text command including the newline

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
  uint32_t ipAddr; // Big endian in_addr: the four bytes are the octets in order
};
#pragma pack(pop)

// Derivation of the NandInfo size, in bytes:
//   8 x uint32_t header fields                 =  32
//   cpuKey + dvdKey (2 x 16)                   =  32  ->   64
//   fuses + vFuses (2 x 0x60)                  = 192  ->  256
//   bl1Key                                     =  16  ->  272
//   bl1Rsa + pirsRsa + masterRsa (3 x 0x110)   = 816  -> 1088
//   cba..cg nonces (6 x 0x10)                  =  96  -> 1184 (0x4A0)
static_assert(sizeof(NandInfo) == 1184, "NandInfo must match the 1184 byte wire layout");
static_assert(std::is_trivially_copyable_v<NandInfo>);
static_assert(sizeof(UdpBcastMsg) == 8, "UdpBcastMsg must match the 8 byte wire layout");
static_assert(sizeof(CommandOp) == sizeof(uint32_t));

// Endianness helpers. Written with shifts so they are constexpr on every compiler
// and still compile to a single byte-swap instruction.
template <std::unsigned_integral T>
requires(sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8)
constexpr T byteSwap(T value) noexcept {
  T result = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    result = static_cast<T>((result << 8) | (value & 0xFF));
    value = static_cast<T>(value >> 8);
  }
  return result;
}

// Converts between host order and big-endian (the operation is its own inverse).
template <std::unsigned_integral T>
requires(sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8)
constexpr T swapBe(T value) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return byteSwap(value);
  } else {
    return value;
  }
}

constexpr uint16_t loadBe16(const uint8_t *p) noexcept {
  return static_cast<uint16_t>((uint16_t{p[0]} << 8) | p[1]);
}

constexpr uint32_t loadBe32(const uint8_t *p) noexcept {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

inline NandInfo swapNandInfo(const NandInfo &in) noexcept {
  NandInfo out = in;
  out.structVer = swapBe(in.structVer);
  out.kernelVer = swapBe(in.kernelVer);
  out.optFlag = swapBe(in.optFlag);
  out.useFlags = swapBe(in.useFlags);
  out.hwFlags = swapBe(in.hwFlags);
  out.dumpSize = swapBe(in.dumpSize);
  out.blockSize = swapBe(in.blockSize);
  out.pairing = swapBe(in.pairing);
  return out;
}

} // namespace updclient::updserver
