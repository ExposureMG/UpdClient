#include "support/mock_transport.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <core/path.hpp>
#include <net/transport_registry.hpp>
#include <protocols/updserver/client.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>

using namespace updclient;
using namespace updclient::updserver;
using ut::Bytes;
using ut::MockScript;

namespace {

struct Rig {
  std::shared_ptr<MockScript> script;
  UpdServerClient client;
};

Rig makeRig(ClientLimits limits = {}) {
  auto script = MockScript::create();
  return Rig{script, UpdServerClient(script->transport(), limits)};
}

Bytes command(uint32_t op) {
  return ut::be32(op);
}

Bytes line(const std::string &text) {
  return ut::bytesOf(text);
}

// 1184 byte NAND_INFO reply: header fields big-endian, the rest recognisable filler.
Bytes nandInfoWire(uint32_t blockSize = 0x4200) {
  Bytes wire = ut::patternBytes(sizeof(NandInfo), 7);
  const uint32_t header[8] = {1, 0x4F5A, 0x20, 3, 4, 0x1000000, blockSize, 0xAABBCCDD};
  for (size_t i = 0; i < 8; ++i) {
    const Bytes field = ut::be32(header[i]);
    std::copy(field.begin(), field.end(), wire.begin() + static_cast<std::ptrdiff_t>(i * 4));
  }
  return wire;
}

void expectUnusable(UpdServerClient &client) {
  CHECK(!client.isConnected());
  CHECK_ERR(client.getVersion(), ErrorCode::NotConnected);
}

} // namespace

TEST(UpdServerWire, CommandNumbersAreStable) {
  CHECK_EQ(static_cast<uint32_t>(CommandOp::Quit), uint32_t{0});
  CHECK_EQ(static_cast<uint32_t>(CommandOp::Shutdown), uint32_t{1});
  CHECK_EQ(static_cast<uint32_t>(CommandOp::Reboot), uint32_t{2});
  CHECK_EQ(static_cast<uint32_t>(CommandOp::SmcReboot), uint32_t{3});
  CHECK_EQ(static_cast<uint32_t>(CommandOp::GetInfo), uint32_t{4});
  CHECK_EQ(static_cast<uint32_t>(CommandOp::GetBbList), uint32_t{5});
  CHECK_EQ(static_cast<uint32_t>(CommandOp::GetFlash), uint32_t{6});
  CHECK_EQ(static_cast<uint32_t>(CommandOp::GetBootloaders), uint32_t{11});
  CHECK_EQ(static_cast<uint32_t>(CommandOp::Get1Bl), uint32_t{24});
  CHECK_EQ(static_cast<uint32_t>(CommandOp::GetVer), uint32_t{26});
  CHECK_EQ(ANNC_PORT, 48);
  CHECK_EQ(NANDSVR_PORT, 49);
}

TEST(UpdServerWire, EndiannessHelpers) {
  CHECK_EQ(byteSwap(uint16_t{0x1234}), uint16_t{0x3412});
  CHECK_EQ(byteSwap(uint32_t{0x01020304}), uint32_t{0x04030201});
  CHECK_EQ(byteSwap(uint64_t{0x0102030405060708ull}), uint64_t{0x0807060504030201ull});
  const uint8_t raw[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  CHECK_EQ(loadBe32(raw), uint32_t{0xDEADBEEF});
  CHECK_EQ(loadBe16(raw), uint16_t{0xDEAD});
  CHECK_EQ(swapBe(swapBe(uint32_t{0x12345678})), uint32_t{0x12345678});
}

TEST(UpdServerClient, GetInfoSendsOpcodeAndDecodesBigEndianHeader) {
  auto rig = makeRig();
  const Bytes wire = nandInfoWire();
  rig.script->reply(wire);

  auto info = rig.client.getInfo();
  REQUIRE_OK(info);
  CHECK_EQ(rig.script->written(), command(4));
  CHECK_EQ(info->structVer, uint32_t{1});
  CHECK_EQ(info->kernelVer, uint32_t{0x4F5A});
  CHECK_EQ(info->optFlag, uint32_t{0x20});
  CHECK_EQ(info->useFlags, uint32_t{3});
  CHECK_EQ(info->hwFlags, uint32_t{4});
  CHECK_EQ(info->dumpSize, uint32_t{0x1000000});
  CHECK_EQ(info->blockSize, uint32_t{0x4200});
  CHECK_EQ(info->pairing, uint32_t{0xAABBCCDD});
  CHECK(std::memcmp(info->cpuKey, wire.data() + offsetof(NandInfo, cpuKey), 16) == 0);
  CHECK(std::memcmp(info->dvdKey, wire.data() + offsetof(NandInfo, dvdKey), 16) == 0);
  CHECK(std::memcmp(info->cgNonce, wire.data() + offsetof(NandInfo, cgNonce), 16) == 0);
  CHECK_EQ(rig.client.knownBlockSize(), uint32_t{0x4200});
  CHECK(rig.client.isConnected());
}

TEST(UpdServerClient, GetInfoAcrossShortReads) {
  auto rig = makeRig();
  rig.script->reply(nandInfoWire()).setMaxRead(100);
  auto info = rig.client.getInfo();
  REQUIRE_OK(info);
  CHECK_EQ(info->blockSize, uint32_t{0x4200});
}

TEST(UpdServerClient, GetInfoTruncatedPayloadFailsAndDropsTheConnection) {
  auto rig = makeRig();
  Bytes wire = nandInfoWire();
  wire.resize(500);
  rig.script->reply(wire);

  const auto info = rig.client.getInfo();
  REQUIRE_ERR(info, ErrorCode::Disconnected);
  CHECK(info.error().message.find("NAND_INFO") != std::string::npos);
  CHECK_EQ(rig.client.knownBlockSize(), uint32_t{0});
  CHECK(rig.script->closed());
  expectUnusable(rig.client);
}

TEST(UpdServerClient, GetInfoTimeoutPropagates) {
  auto rig = makeRig();
  rig.script->reply(Bytes(200, 0)).replyTimeout();
  CHECK_ERR(rig.client.getInfo(), ErrorCode::Timeout);
  CHECK(!rig.client.isConnected());
}

TEST(UpdServerClient, GetVersionDecodesPackedVersion) {
  auto rig = makeRig();
  rig.script->reply(ut::be32(0x00010203));
  auto version = rig.client.getVersion();
  REQUIRE_OK(version);
  CHECK_EQ(*version, std::string("1.2.3"));
  CHECK_EQ(rig.script->written(), command(26));
}

TEST(UpdServerClient, GetVersionIgnoresTheTopByte) {
  auto rig = makeRig();
  rig.script->reply(ut::be32(0xFF0A0B0C));
  auto version = rig.client.getVersion();
  REQUIRE_OK(version);
  CHECK_EQ(*version, std::string("10.11.12"));
}

TEST(UpdServerClient, GetVersionTruncated) {
  auto rig = makeRig();
  rig.script->reply(Bytes{0x00, 0x01});
  CHECK_ERR(rig.client.getVersion(), ErrorCode::Disconnected);
}

TEST(UpdServerClient, GetBadBlockListDecodesBigEndianBlocks) {
  auto rig = makeRig();
  rig.script->reply(ut::concat({ut::be32(3), ut::be16(0x0001), ut::be16(0x1234), ut::be16(0xFFFF)}));
  auto list = rig.client.getBadBlockList();
  REQUIRE_OK(list);
  CHECK_EQ(rig.script->written(), command(5));
  CHECK_EQ(*list, (std::vector<uint16_t>{0x0001, 0x1234, 0xFFFF}));
}

TEST(UpdServerClient, GetBadBlockListEmpty) {
  auto rig = makeRig();
  rig.script->reply(ut::be32(0));
  auto list = rig.client.getBadBlockList();
  REQUIRE_OK(list);
  CHECK(list->empty());
  CHECK(rig.client.isConnected());
}

TEST(UpdServerClient, GetBadBlockListOversizedCountIsLimitExceeded) {
  auto rig = makeRig();
  rig.script->reply(ut::be32(0x10001));
  const auto list = rig.client.getBadBlockList();
  REQUIRE_ERR(list, ErrorCode::LimitExceeded);
  CHECK(!rig.client.isConnected());
}

TEST(UpdServerClient, GetBadBlockListHugeCountDoesNotAllocate) {
  auto rig = makeRig();
  rig.script->reply(ut::be32(0xFFFFFFFFu));
  CHECK_ERR(rig.client.getBadBlockList(), ErrorCode::LimitExceeded);
}

TEST(UpdServerClient, GetBadBlockListHonoursCustomLimit) {
  ClientLimits limits;
  limits.maxBadBlockCount = 3;
  {
    auto rig = makeRig(limits);
    rig.script->reply(ut::concat({ut::be32(3), ut::be16(1), ut::be16(2), ut::be16(3)}));
    auto list = rig.client.getBadBlockList();
    REQUIRE_OK(list);
    CHECK_EQ(list->size(), size_t{3});
  }
  {
    auto rig = makeRig(limits);
    rig.script->reply(ut::be32(4));
    CHECK_ERR(rig.client.getBadBlockList(), ErrorCode::LimitExceeded);
  }
}

TEST(UpdServerClient, GetBadBlockListTruncatedList) {
  auto rig = makeRig();
  rig.script->reply(ut::concat({ut::be32(5), ut::be16(1), ut::be16(2)}));
  CHECK_ERR(rig.client.getBadBlockList(), ErrorCode::Disconnected);
  CHECK(!rig.client.isConnected());
}

TEST(UpdServerClient, PeekSendsTextCommandAndReturnsRawBytes) {
  auto rig = makeRig();
  const Bytes data = ut::patternBytes(4, 1);
  rig.script->reply(data);
  auto out = rig.client.peek(0x1000, 4);
  REQUIRE_OK(out);
  CHECK_EQ(rig.script->writtenText(), std::string("PEEK 00001000 00000004\n"));
  CHECK_EQ(*out, data);
}

TEST(UpdServerClient, PeekTruncatedReply) {
  auto rig = makeRig();
  rig.script->reply(ut::patternBytes(10));
  CHECK_ERR(rig.client.peek(0, 16), ErrorCode::Disconnected);
  CHECK(!rig.client.isConnected());
}

TEST(UpdServerClient, PeekRejectsBadArgumentsWithoutSending) {
  auto rig = makeRig();
  CHECK_ERR(rig.client.peek(0, 0), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.peek(0xFFFFFFF0u, 0x20), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.peek(0, kMaxPeekBytes + 1), ErrorCode::LimitExceeded);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
  CHECK(rig.client.isConnected());
}

TEST(UpdServerClient, PeekAtTheTopOfTheAddressSpaceIsAllowed) {
  auto rig = makeRig();
  rig.script->reply(ut::patternBytes(16));
  CHECK_OK(rig.client.peek(0xFFFFFFF0u, 16));
  CHECK_EQ(rig.script->writtenText(), std::string("PEEK FFFFFFF0 00000010\n"));
}

TEST(UpdServerClient, PokeSendsTextCommand) {
  auto rig = makeRig();
  REQUIRE_OK(rig.client.poke(0x1000, 0xDEADBEEF));
  CHECK_EQ(rig.script->writtenText(), std::string("POKE 00001000 DEADBEEF\n"));
  REQUIRE_OK(rig.client.poke(0, 0));
  CHECK_EQ(rig.script->writtenText(), std::string("POKE 00001000 DEADBEEF\nPOKE 00000000 00000000\n"));
}

TEST(UpdServerClient, HvPeekAndHvPokeUseSixteenDigitAddresses) {
  auto rig = makeRig();
  rig.script->reply(ut::patternBytes(8));
  REQUIRE_OK(rig.client.hvPeek(0x0000000180000000ull, 8));
  REQUIRE_OK(rig.client.hvPoke(0x1122334455667788ull, 0xAABBCCDDEEFF0011ull));
  CHECK_EQ(rig.script->writtenText(),
           std::string("HVPE 0000000180000000 00000008\nHVPO 1122334455667788 AABBCCDDEEFF0011\n"));
}

TEST(UpdServerClient, HvPeekRejectsWrapAround) {
  auto rig = makeRig();
  CHECK_ERR(rig.client.hvPeek(0xFFFFFFFFFFFFFFF8ull, 16), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.hvPeek(0, 0), ErrorCode::InvalidArgument);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
}

TEST(UpdServerClient, Get1blReadsExactlyThe1blSize) {
  auto rig = makeRig();
  const Bytes data = ut::patternBytes(0x8000, 3);
  rig.script->reply(data);
  auto out = rig.client.get1bl();
  REQUIRE_OK(out);
  CHECK_EQ(rig.script->written(), command(24));
  CHECK_EQ(*out, data);
}

TEST(UpdServerClient, Get1blTruncated) {
  auto rig = makeRig();
  rig.script->reply(ut::patternBytes(0x7FFF));
  CHECK_ERR(rig.client.get1bl(), ErrorCode::Disconnected);
}

TEST(UpdServerClient, GetBootloadersReadsLengthPrefixedPayload) {
  auto rig = makeRig();
  const Bytes data = ut::patternBytes(5000, 5);
  rig.script->reply(ut::be32(5000)).reply(data);
  auto out = rig.client.getBootloaders();
  REQUIRE_OK(out);
  CHECK_EQ(rig.script->written(), command(11));
  CHECK_EQ(*out, data);
}

TEST(UpdServerClient, GetBootloadersRejectsEmptyAndOversizedReplies) {
  {
    auto rig = makeRig();
    rig.script->reply(ut::be32(0));
    CHECK_ERR(rig.client.getBootloaders(), ErrorCode::Protocol);
  }
  {
    auto rig = makeRig();
    rig.script->reply(ut::be32(static_cast<uint32_t>(kMaxBootloaderBytes) + 1));
    CHECK_ERR(rig.client.getBootloaders(), ErrorCode::LimitExceeded);
  }
}

TEST(UpdServerClient, ReadBlockSendsHexRangeAndReadsSizedPayload) {
  auto rig = makeRig();
  const Bytes data = ut::patternBytes(0x4200 * 2, 9);
  rig.script->reply(ut::be32(static_cast<uint32_t>(data.size()))).replyChunked(data, 5000);
  auto out = rig.client.readBlock(0x1F, 2);
  REQUIRE_OK(out);
  CHECK_EQ(rig.script->writtenText(), std::string("RBLK 1F 2\n"));
  CHECK_EQ(*out, data);
}

TEST(UpdServerClient, ReadBlockDefaultsToOneBlock) {
  auto rig = makeRig();
  rig.script->reply(ut::be32(4)).reply(ut::patternBytes(4));
  REQUIRE_OK(rig.client.readBlock(5));
  CHECK_EQ(rig.script->writtenText(), std::string("RBLK 5 1\n"));
}

TEST(UpdServerClient, ReadBlockZeroIsEncodedAsSingleDigit) {
  auto rig = makeRig();
  rig.script->reply(ut::be32(1)).reply(Bytes{0x42});
  REQUIRE_OK(rig.client.readBlock(0, 1));
  CHECK_EQ(rig.script->writtenText(), std::string("RBLK 0 1\n"));
}

TEST(UpdServerClient, ReadBlockOversizedWireCountIsLimitExceeded) {
  ClientLimits limits;
  limits.maxBlockPayload = 1024;
  auto rig = makeRig(limits);
  rig.script->reply(ut::be32(1025));
  const auto out = rig.client.readBlock(0, 1);
  REQUIRE_ERR(out, ErrorCode::LimitExceeded);
  CHECK(!rig.client.isConnected());
}

TEST(UpdServerClient, ReadBlockDefaultLimitRejectsGiganticAnnouncement) {
  auto rig = makeRig();
  rig.script->reply(ut::be32(0xFFFFFFFFu));
  CHECK_ERR(rig.client.readBlock(0, 1), ErrorCode::LimitExceeded);
}

TEST(UpdServerClient, ReadBlockEmptyReplyIsAProtocolError) {
  auto rig = makeRig();
  rig.script->reply(ut::be32(0));
  CHECK_ERR(rig.client.readBlock(0, 1), ErrorCode::Protocol);
}

TEST(UpdServerClient, ReadBlockTruncatedPayload) {
  auto rig = makeRig();
  rig.script->reply(ut::be32(1000)).reply(ut::patternBytes(400));
  CHECK_ERR(rig.client.readBlock(0, 1), ErrorCode::Disconnected);
  CHECK(!rig.client.isConnected());
}

TEST(UpdServerClient, ReadBlockValidatesTheRange) {
  auto rig = makeRig();
  CHECK_ERR(rig.client.readBlock(0, 0), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.readBlock(0, kMaxBlockCount + 1), ErrorCode::LimitExceeded);
  CHECK_ERR(rig.client.readBlock(0xFFFFFFFFu, 2), ErrorCode::InvalidArgument);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
  CHECK(rig.client.isConnected());
}

TEST(UpdServerClient, WriteBlockSendsHeaderThenPayload) {
  auto rig = makeRig();
  const Bytes data = ut::patternBytes(64, 2);
  REQUIRE_OK(rig.client.writeBlock(5, data));
  CHECK_EQ(rig.script->written(), ut::concat({line("WBLK 5 1\n"), data}));
}

TEST(UpdServerClient, WriteBlockWithPartialWrites) {
  auto rig = makeRig();
  rig.script->setMaxWrite(3);
  const Bytes data = ut::patternBytes(64, 3);
  REQUIRE_OK(rig.client.writeBlock(0xABCD, data));
  CHECK_EQ(rig.script->written(), ut::concat({line("WBLK ABCD 1\n"), data}));
}

TEST(UpdServerClient, WriteBlockEnforcesTheKnownBlockSize) {
  auto rig = makeRig();
  rig.script->reply(nandInfoWire(0x4200));
  REQUIRE_OK(rig.client.getInfo());
  const size_t before = rig.script->written().size();

  CHECK_ERR(rig.client.writeBlock(1, ut::patternBytes(0x4200 - 1)), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.writeBlock(1, ut::patternBytes(0x4200 + 1)), ErrorCode::InvalidArgument);
  CHECK_EQ(rig.script->written().size(), before);

  const Bytes block = ut::patternBytes(0x4200, 4);
  REQUIRE_OK(rig.client.writeBlock(1, block));
  CHECK_EQ(rig.script->written().size(), before + 9 + block.size());
}

TEST(UpdServerClient, WriteBlockRejectsEmptyAndOversizedData) {
  auto rig = makeRig();
  CHECK_ERR(rig.client.writeBlock(0, {}), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.writeBlock(0, ut::patternBytes(kMaxBlockWriteBytes + 1)), ErrorCode::LimitExceeded);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
}

TEST(UpdServerClient, EraseBlockSendsHexRange) {
  auto rig = makeRig();
  REQUIRE_OK(rig.client.eraseBlock(0x10, 3));
  REQUIRE_OK(rig.client.eraseBlock(7));
  CHECK_EQ(rig.script->writtenText(), std::string("ERBL 10 3\nERBL 7 1\n"));
  CHECK_ERR(rig.client.eraseBlock(0, 0), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.eraseBlock(0xFFFFFFFFu, 2), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.eraseBlock(0, kMaxBlockCount + 1), ErrorCode::LimitExceeded);
}

TEST(UpdServerClient, ControlCommandsAreFourByteBigEndianOpcodes) {
  auto rig = makeRig();
  REQUIRE_OK(rig.client.quit());
  REQUIRE_OK(rig.client.shutdownConsole());
  REQUIRE_OK(rig.client.reboot());
  REQUIRE_OK(rig.client.smcReboot());
  CHECK_EQ(rig.script->written(), (Bytes{0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3}));
}

TEST(UpdServerClient, MountUnmountMkDirTextCommands) {
  auto rig = makeRig();
  REQUIRE_OK(rig.client.mount("hdd:", "\\Device\\Harddisk0\\Partition1"));
  REQUIRE_OK(rig.client.unmount("hdd:"));
  REQUIRE_OK(rig.client.mkDir("hdd:\\my dir"));
  CHECK_EQ(rig.script->writtenText(),
           std::string("MTPT hdd: \\Device\\Harddisk0\\Partition1\nUMPT hdd:\nMKDR hdd:\\my dir\n"));
}

TEST(UpdServerClient, TextArgumentsCannotInjectCommands) {
  auto rig = makeRig();
  CHECK_ERR(rig.client.mkDir("a\nREBOOT"), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.mkDir("a\rb"), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.mkDir(std::string("a\0b", 3)), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.mkDir(""), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.unmount("x\ny"), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.mount("hd d:", "dev"), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.mount("hdd:", "dev\nx"), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.mount("", "dev"), ErrorCode::InvalidArgument);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
  CHECK(rig.client.isConnected());
}

TEST(UpdServerClient, OverlongCommandLineIsLimitExceeded) {
  auto rig = makeRig();
  CHECK_ERR(rig.client.mkDir(std::string(2000, 'a')), ErrorCode::LimitExceeded);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
}

TEST(UpdServerClient, SendFailureFromRebootPropagates) {
  auto rig = makeRig();
  rig.script->failWrites(makeError(ErrorCode::Disconnected, "peer reset", 104));
  const auto r = rig.client.reboot();
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK_EQ(r.error().sysError, 104);
  CHECK(r.error().message.find("REBOOT") != std::string::npos);
  expectUnusable(rig.client);
}

TEST(UpdServerClient, SendFailureFromPokePropagates) {
  auto rig = makeRig();
  rig.script->failWrites(makeError(ErrorCode::Io, "broken pipe", 32));
  const auto r = rig.client.poke(0x1000, 1);
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK_EQ(r.error().sysError, 32);
  expectUnusable(rig.client);
}

TEST(UpdServerClient, EveryFireAndForgetCommandPropagatesSendFailures) {
  using Call = std::function<Result<void>(UpdServerClient &)>;
  const std::vector<std::pair<const char *, Call>> calls = {
      {"quit", [](UpdServerClient &c) { return c.quit(); }},
      {"shutdown", [](UpdServerClient &c) { return c.shutdownConsole(); }},
      {"reboot", [](UpdServerClient &c) { return c.reboot(); }},
      {"smcReboot", [](UpdServerClient &c) { return c.smcReboot(); }},
      {"poke", [](UpdServerClient &c) { return c.poke(1, 2); }},
      {"hvPoke", [](UpdServerClient &c) { return c.hvPoke(1, 2); }},
      {"mount", [](UpdServerClient &c) { return c.mount("a:", "dev"); }},
      {"unmount", [](UpdServerClient &c) { return c.unmount("a:"); }},
      {"mkDir", [](UpdServerClient &c) { return c.mkDir("a:\\x"); }},
      {"eraseBlock", [](UpdServerClient &c) { return c.eraseBlock(1, 1); }},
      {"writeBlock", [](UpdServerClient &c) { return c.writeBlock(1, Bytes(16, 1)); }},
  };
  for (const auto &[name, call] : calls) {
    auto rig = makeRig();
    rig.script->failWrites(makeError(ErrorCode::Io, "write failed"));
    const auto r = call(rig.client);
    CHECK_MSG(!r.has_value(), name);
    if (!r) CHECK_EQ(r.error().code, ErrorCode::Io);
    CHECK_MSG(!rig.client.isConnected(), name);
  }
}

TEST(UpdServerClient, PartialWriteThenFailureIsReportedAndConnectionDropped) {
  auto rig = makeRig();
  rig.script->failWritesAfter(2, makeError(ErrorCode::Io, "cut off"));
  CHECK_ERR(rig.client.reboot(), ErrorCode::Io);
  CHECK_EQ(rig.script->written().size(), size_t{2});
  CHECK(!rig.client.isConnected());
}

TEST(UpdServerClient, CommandsFailCleanlyAfterDisconnect) {
  auto rig = makeRig();
  rig.client.disconnect();
  CHECK(!rig.client.isConnected());
  CHECK_ERR(rig.client.getInfo(), ErrorCode::NotConnected);
  CHECK_ERR(rig.client.reboot(), ErrorCode::NotConnected);
  CHECK_ERR(rig.client.peek(0, 4), ErrorCode::NotConnected);
  CHECK_ERR(rig.client.mkDir("a"), ErrorCode::NotConnected);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
}

TEST(UpdServerClient, MovedFromClientHasNoTransport) {
  auto rig = makeRig();
  UpdServerClient moved(std::move(rig.client));
  CHECK(moved.isConnected());
  CHECK_EQ(moved.describe(), std::string("mock"));
  CHECK(!rig.client.isConnected());  // NOLINT(bugprone-use-after-move)
  CHECK_EQ(rig.client.describe(), std::string("<no transport>"));
  CHECK_ERR(rig.client.getVersion(), ErrorCode::NotConnected);
  rig.script->reply(ut::be32(0x00000001));
  auto version = moved.getVersion();
  REQUIRE_OK(version);
  CHECK_EQ(*version, std::string("0.0.1"));
}

TEST(UpdServerClient, TwoCommandsBackToBackOnOneConnection) {
  auto rig = makeRig();
  rig.script->reply(ut::be32(0x00020000)).reply(ut::concat({ut::be32(1), ut::be16(9)}));
  auto version = rig.client.getVersion();
  REQUIRE_OK(version);
  CHECK_EQ(*version, std::string("2.0.0"));
  auto bad = rig.client.getBadBlockList();
  REQUIRE_OK(bad);
  CHECK_EQ(*bad, std::vector<uint16_t>{9});
  CHECK_EQ(rig.script->written(), ut::concat({command(26), command(5)}));
}

TEST(UpdServerClient, LimitsAreReadableAndChangeable) {
  auto rig = makeRig();
  CHECK_EQ(rig.client.limits().maxBadBlockCount, kMaxBadBlockCount);
  ClientLimits limits = rig.client.limits();
  limits.maxPeekBytes = 8;
  rig.client.setLimits(limits);
  CHECK_ERR(rig.client.peek(0, 9), ErrorCode::LimitExceeded);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
}

TEST(UpdServerGetFile, DownloadsToTheLocalFileAndLeavesNoPartFile) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto rig = makeRig();
  const Bytes content = ut::patternBytes(3000, 21);
  rig.script->reply(ut::be32(3000)).replyChunked(content, 700);

  std::vector<size_t> progress;
  const auto local = dir.file("hosts.txt").string();
  REQUIRE_OK(rig.client.getFile("hdd:\\a b.bin", local, [&](size_t n) { progress.push_back(n); }));

  CHECK_EQ(rig.script->writtenText(), std::string("GETF hdd:\\a b.bin\n"));
  auto onDisk = ut::readFile(local);
  REQUIRE(onDisk.has_value());
  CHECK_EQ(*onDisk, content);
  CHECK_EQ(dir.entries(), std::vector<std::string>{"hosts.txt"});
  CHECK_EQ(progress, (std::vector<size_t>{1452, 2904, 3000}));
  CHECK(rig.client.isConnected());
}

TEST(UpdServerGetFile, EmptyRemoteFile) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto rig = makeRig();
  rig.script->reply(ut::be32(0));
  const auto local = dir.file("empty").string();
  REQUIRE_OK(rig.client.getFile("x", local));
  auto onDisk = ut::readFile(local);
  REQUIRE(onDisk.has_value());
  CHECK(onDisk->empty());
}

TEST(UpdServerGetFile, TruncatedDownloadLeavesNothingUnderTheFinalName) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto rig = makeRig();
  rig.script->reply(ut::be32(5000)).reply(ut::patternBytes(3100));
  const auto local = dir.file("out.bin").string();

  const auto r = rig.client.getFile("remote.bin", local);
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK(!std::filesystem::exists(local));
  CHECK(dir.entries().empty());
  CHECK(!rig.client.isConnected());
}

TEST(UpdServerGetFile, FailureKeepsAPreviousFileIntact) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const auto local = dir.file("out.bin");
  const Bytes previous = ut::bytesOf("previous content");
  REQUIRE(ut::writeFile(local, previous));

  auto rig = makeRig();
  rig.script->reply(ut::be32(5000)).reply(ut::patternBytes(100)).replyTimeout();
  CHECK_ERR(rig.client.getFile("remote.bin", local.string()), ErrorCode::Timeout);
  auto onDisk = ut::readFile(local);
  REQUIRE(onDisk.has_value());
  CHECK_EQ(*onDisk, previous);
  CHECK_EQ(dir.entries(), std::vector<std::string>{"out.bin"});
}

TEST(UpdServerGetFile, AnnouncedSizeOverLimitIsRejected) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  ClientLimits limits;
  limits.maxFileBytes = 1000;
  auto rig = makeRig(limits);
  rig.script->reply(ut::be32(1001));
  CHECK_ERR(rig.client.getFile("x", dir.file("o").string()), ErrorCode::LimitExceeded);
  CHECK(dir.entries().empty());
  CHECK(!rig.client.isConnected());
}

TEST(UpdServerGetFile, LocalProblemsAreReportedBeforeAnythingIsSent) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto rig = makeRig();
  CHECK_ERR(rig.client.getFile("x", dir.path().string()), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.getFile("x", ""), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.getFile("x", dir.file("no-such-dir/out").string()), ErrorCode::Io);
  CHECK_ERR(rig.client.getFile("", dir.file("o").string()), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.getFile("bad\nname", dir.file("o").string()), ErrorCode::InvalidArgument);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
  CHECK(rig.client.isConnected());
  CHECK(dir.entries().empty());
}

TEST(UpdServerSendFile, UploadsHeaderSizeAndContent) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const Bytes content = ut::patternBytes(3000, 31);
  const auto local = dir.file("up.bin");
  REQUIRE(ut::writeFile(local, content));

  auto rig = makeRig();
  rig.script->setMaxWrite(400);
  std::vector<size_t> progress;
  REQUIRE_OK(rig.client.sendFile(local.string(), "hdd:\\up.bin", [&](size_t n) { progress.push_back(n); }));

  CHECK_EQ(rig.script->written(), ut::concat({line("SNDF hdd:\\up.bin\n"), ut::be32(3000), content}));
  CHECK_EQ(progress, (std::vector<size_t>{1452, 2904, 3000}));
  CHECK(rig.client.isConnected());
}

TEST(UpdServerSendFile, EmptyFileSendsOnlyTheHeader) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const auto local = dir.file("empty");
  REQUIRE(ut::writeFile(local, {}));
  auto rig = makeRig();
  REQUIRE_OK(rig.client.sendFile(local.string(), "r"));
  CHECK_EQ(rig.script->written(), ut::concat({line("SNDF r\n"), ut::be32(0)}));
}

TEST(UpdServerSendFile, FileSizeIsBigEndian) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const Bytes content = ut::patternBytes(0x0102, 1);
  const auto local = dir.file("f");
  REQUIRE(ut::writeFile(local, content));
  auto rig = makeRig();
  REQUIRE_OK(rig.client.sendFile(local.string(), "r"));
  const Bytes &w = rig.script->written();
  REQUIRE(w.size() >= 11);
  CHECK_EQ(Bytes(w.begin() + 7, w.begin() + 11), (Bytes{0x00, 0x00, 0x01, 0x02}));
}

TEST(UpdServerSendFile, WriteFailureMidUploadDropsTheConnection) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const auto local = dir.file("up.bin");
  REQUIRE(ut::writeFile(local, ut::patternBytes(6000)));
  auto rig = makeRig();
  rig.script->failWritesAfter(2000, makeError(ErrorCode::Disconnected, "reset"));
  CHECK_ERR(rig.client.sendFile(local.string(), "r"), ErrorCode::Disconnected);
  CHECK(!rig.client.isConnected());
}

TEST(UpdServerSendFile, MissingOrNonRegularLocalFileIsRejectedBeforeSending) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto rig = makeRig();
  CHECK_ERR(rig.client.sendFile(dir.file("missing").string(), "r"), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.sendFile(dir.path().string(), "r"), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.sendFile("", "r"), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.sendFile(dir.file("x").string(), "bad\nname"), ErrorCode::InvalidArgument);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
  CHECK(rig.client.isConnected());
}

TEST(UpdServerSendFile, FileOverTheConfiguredLimitIsRejected) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const auto local = dir.file("big");
  REQUIRE(ut::writeFile(local, ut::patternBytes(100)));
  ClientLimits limits;
  limits.maxFileBytes = 99;
  auto rig = makeRig(limits);
  CHECK_ERR(rig.client.sendFile(local.string(), "r"), ErrorCode::LimitExceeded);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
  CHECK(rig.client.isConnected());
}

// There is no size seam in the client, so the 4 GiB limit is exercised with a real
// sparse file. Filesystems that cannot create one make the test skip.
TEST(UpdServerSendFile, FileOfFourGiBOrMoreIsRejected) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const auto local = dir.file("huge.bin");
  {
    std::ofstream create(local, std::ios::binary);
    REQUIRE(static_cast<bool>(create));
  }
  std::error_code ec;
  std::filesystem::resize_file(local, uint64_t{1} << 32, ec);
  if (ec) SKIP("cannot create a sparse 4 GiB file here: " + ec.message());
  if (std::filesystem::file_size(local, ec) != (uint64_t{1} << 32)) SKIP("filesystem did not keep the 4 GiB size");

  auto rig = makeRig();
  const auto r = rig.client.sendFile(local.string(), "r");
  REQUIRE_ERR(r, ErrorCode::LimitExceeded);
  CHECK(r.error().message.find("4 GiB") != std::string::npos);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
  CHECK(rig.client.isConnected());
}

TEST(UpdServerDumpFlash, NonAsciiPathFromUtf8) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto rig = makeRig();
  const Bytes dump = ut::patternBytes(3000, 5);
  rig.script->reply(dump);

  const std::string utf8Name = "nand-jos\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC.bin";
  const auto out = dir.path() / pathFromUtf8(utf8Name);
  REQUIRE_OK(rig.client.dumpFlash(out, dump.size()));
  auto onDisk = ut::readFile(out);
  REQUIRE(onDisk.has_value());
  CHECK_EQ(*onDisk, dump);

  rig.script->reply(ut::be32(0));
  const auto badDir = dir.path() / pathFromUtf8("no-such-j\xC3\xB6-dir") / "x.bin";
  const auto r = rig.client.getFile("remote", badDir);
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(r.error().message.find("no-such-j\xC3\xB6-dir") != std::string::npos);
}

TEST(UpdServerDumpFlash, StreamsTheDumpToDisk) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto rig = makeRig();
  const size_t size = 64 * 1024 + 4321;
  const Bytes dump = ut::patternBytes(size, 77);
  rig.script->replyChunked(dump, 10000);

  std::vector<std::pair<size_t, size_t>> progress;
  const auto out = dir.file("nand.bin").string();
  REQUIRE_OK(rig.client.dumpFlash(out, size, [&](size_t got, size_t total) { progress.emplace_back(got, total); }));

  CHECK_EQ(rig.script->written(), command(6));
  auto onDisk = ut::readFile(out);
  REQUIRE(onDisk.has_value());
  CHECK_EQ(*onDisk, dump);
  REQUIRE(!progress.empty());
  CHECK_EQ(progress.front(), (std::pair<size_t, size_t>{64 * 1024, size}));
  CHECK_EQ(progress.back(), (std::pair<size_t, size_t>{size, size}));
  CHECK_EQ(dir.entries(), std::vector<std::string>{"nand.bin"});
}

TEST(UpdServerDumpFlash, TruncatedDumpLeavesNoFile) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto rig = makeRig();
  rig.script->reply(ut::patternBytes(70000));
  const auto r = rig.client.dumpFlash(dir.file("nand.bin").string(), 200000);
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK(dir.entries().empty());
  CHECK(!rig.client.isConnected());
}

TEST(UpdServerDumpFlash, FailureKeepsAPreviousDumpIntact) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const auto out = dir.file("nand.bin");
  const Bytes previous = ut::patternBytes(64, 1);
  REQUIRE(ut::writeFile(out, previous));
  auto rig = makeRig();
  rig.script->reply(ut::patternBytes(100)).replyTimeout();
  CHECK_ERR(rig.client.dumpFlash(out.string(), 5000), ErrorCode::Timeout);
  auto onDisk = ut::readFile(out);
  REQUIRE(onDisk.has_value());
  CHECK_EQ(*onDisk, previous);
}

TEST(UpdServerDumpFlash, RejectsBadSizesBeforeSending) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  ClientLimits limits;
  limits.maxDumpBytes = 1000;
  auto rig = makeRig(limits);
  CHECK_ERR(rig.client.dumpFlash(dir.file("o").string(), 0), ErrorCode::InvalidArgument);
  CHECK_ERR(rig.client.dumpFlash(dir.file("o").string(), 1001), ErrorCode::LimitExceeded);
  CHECK_ERR(rig.client.dumpFlash("", 10), ErrorCode::InvalidArgument);
  CHECK_EQ(rig.script->writeCalls(), size_t{0});
  CHECK(dir.entries().empty());
}

TEST(UpdServerConnect, ConnectsThroughTheTransportRegistry) {
  auto script = MockScript::create();
  net::Endpoint seen;
  net::SchemeTraits traits;
  traits.usesProtocolPort = true;
  net::TransportRegistry::instance().registerScheme("mockupd", [&](const net::Endpoint &e) -> Result<net::TransportPtr> {
    seen = e;
    return net::TransportPtr(script->transport());
  }, traits);

  net::Endpoint endpoint;
  endpoint.scheme = "mockupd";
  endpoint.host = "console";
  auto client = UpdServerClient::connect(endpoint);
  REQUIRE_OK(client);
  CHECK_EQ(seen.port, NANDSVR_PORT);
  CHECK_EQ(seen.host, std::string("console"));
  CHECK(client->isConnected());

  script->reply(ut::be32(0x00000102));
  auto version = client->getVersion();
  REQUIRE_OK(version);
  CHECK_EQ(*version, std::string("0.1.2"));

  endpoint.port = 4949;
  auto second = UpdServerClient::connect(endpoint);
  REQUIRE_OK(second);
  CHECK_EQ(seen.port, 4949);

  net::TransportRegistry::instance().unregisterScheme("mockupd");
}

TEST(UpdServerConnect, PortlessSchemesKeepPortZero) {
  auto script = MockScript::create();
  net::Endpoint seen;
  net::TransportRegistry::instance().registerScheme("mockserial", [&](const net::Endpoint &e) -> Result<net::TransportPtr> {
    seen = e;
    return net::TransportPtr(script->transport());
  });

  auto endpoint = net::Endpoint::parse("mockserial:///dev/ttyUSB0?baud=115200");
  REQUIRE_OK(endpoint);
  REQUIRE_OK(UpdServerClient::connect(*endpoint));
  CHECK_EQ(seen.port, 0);
  CHECK_EQ(seen.host, std::string("/dev/ttyUSB0"));
  CHECK_EQ(seen.options.at("baud"), std::string("115200"));
  CHECK_EQ(seen.toString(), std::string("mockserial:///dev/ttyUSB0?baud=115200"));
  net::TransportRegistry::instance().unregisterScheme("mockserial");
}

TEST(UpdServerConnect, SchemeDefaultPortBeatsTheProtocolPort) {
  auto script = MockScript::create();
  net::Endpoint seen;
  net::SchemeTraits traits;
  traits.defaultPort = 443;
  traits.usesProtocolPort = true;
  net::TransportRegistry::instance().registerScheme("mocktls", [&](const net::Endpoint &e) -> Result<net::TransportPtr> {
    seen = e;
    return net::TransportPtr(script->transport());
  }, traits);

  auto endpoint = net::Endpoint::parse("mocktls://console");
  REQUIRE_OK(endpoint);
  REQUIRE_OK(UpdServerClient::connect(*endpoint));
  CHECK_EQ(seen.port, 443);
  net::TransportRegistry::instance().unregisterScheme("mocktls");
}

TEST(UpdServerConnect, UnknownSchemeIsUnsupported) {
  net::Endpoint endpoint;
  endpoint.scheme = "no-such-transport";
  endpoint.host = "console";
  CHECK_ERR(UpdServerClient::connect(endpoint), ErrorCode::Unsupported);
}

TEST(UpdServerConnect, ConnectorFailureIsReported) {
  net::TransportRegistry::instance().registerScheme("mockupdfail", [](const net::Endpoint &) -> Result<net::TransportPtr> {
    return fail(ErrorCode::ConnectFailed, "refused", 111);
  });
  net::Endpoint endpoint;
  endpoint.scheme = "mockupdfail";
  endpoint.host = "console";
  const auto client = UpdServerClient::connect(endpoint);
  REQUIRE_ERR(client, ErrorCode::ConnectFailed);
  CHECK_EQ(client.error().sysError, 111);
  net::TransportRegistry::instance().unregisterScheme("mockupdfail");
}

TEST(UpdServerConnect, NullTransportFromConnectorIsAConnectFailure) {
  net::TransportRegistry::instance().registerScheme("mockupdnull", [](const net::Endpoint &) -> Result<net::TransportPtr> {
    return net::TransportPtr();
  });
  net::Endpoint endpoint;
  endpoint.scheme = "mockupdnull";
  endpoint.host = "console";
  CHECK_ERR(UpdServerClient::connect(endpoint), ErrorCode::ConnectFailed);
  net::TransportRegistry::instance().unregisterScheme("mockupdnull");
}
