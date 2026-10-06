#include "protocols/xbdm/integration_support.hpp"

#include <updclient/protocols/xbdm/path.hpp>

#include <algorithm>
#include <atomic>
#include <thread>

using namespace xit;
using updclient::xbdm::consoleStatusCode;
using updclient::xbdm::ExecState;
using updclient::xbdm::PowerResult;
using updclient::xbdm::RebootMode;

namespace {

template <class R> std::optional<int> statusOf(const R &result) {
  if (result) return std::nullopt;
  return consoleStatusCode(result.error());
}

} // namespace

XBDM_LINK_TEST(XbdmIntegration, ConsoleInformation) {
  Rig rig(link);
  auto client = rig.client();
  auto info = client.consoleInfo();
  REQUIRE_OK(info);
  CHECK_EQ(info->debugName.value_or(""), std::string("MockDevkit"));
  CHECK_EQ(info->consoleType.value_or(""), std::string("devkit"));
  CHECK_EQ(info->consoleId.value_or(""), std::string("0123456789ab"));
  REQUIRE(info->runningTitle.has_value());
  CHECK_EQ(info->runningTitle->name, std::string("\\Device\\Harddisk0\\Partition1\\DEVKIT\\Mock\\default.xex"));
  CHECK_EQ(info->runningTitle->timestamp.value_or(1), 0u);
  REQUIRE(info->execState.has_value());
  CHECK(info->execState->state == ExecState::Start);
  REQUIRE(info->titleAddress.has_value());
  CHECK_EQ(info->titleAddress->raw, 0xC0A80102u);
  CHECK_EQ(info->titleAddress->text, std::string("192.168.1.2"));

  auto xex = client.executableInfo("HDD:\\Games\\Mock\\default.xex");
  REQUIRE_OK(xex);
  CHECK_EQ(xex->name, std::string("HDD:\\Games\\Mock\\default.xex"));
  CHECK_EQ(xex->timestamp.value_or(0), 0x4b1d2a3cu);
  CHECK_EQ(statusOf(client.executableInfo("HDD:\\nope.xex")).value_or(0), 402);
  CHECK(client.isConnected());

  CHECK_EQ(rig.linesNamed("dbgname"), (std::vector<std::string>{"dbgname"}));
  CHECK_EQ(rig.linesNamed("xbeinfo").front(), std::string("xbeinfo running"));
  rig.checkCleanTraffic();
}

XBDM_LINK_TEST(XbdmIntegration, RefusedInformationIsLeftOut) {
  Rig rig(link);
  auto info = rig.mock.info();
  info.runningTitle.clear();
  rig.mock.setInfo(info);
  rig.mock.inject(XbdmFault::statusLine("407- unknown command").on("altaddr"));
  auto client = rig.client();
  auto all = client.consoleInfo();
  REQUIRE_OK(all);
  CHECK(!all->runningTitle.has_value());
  CHECK(!all->titleAddress.has_value());
  CHECK(all->debugName.has_value());
  CHECK(all->execState.has_value());
  auto title = client.runningTitle();
  CHECK_ERR(title, ErrorCode::Io);
  CHECK_EQ(statusOf(title).value_or(0), 402);
  CHECK(client.isConnected());
}

XBDM_LINK_TEST(XbdmIntegration, ExecutionStates) {
  Rig rig(link);
  auto client = rig.client();
  for (const auto &[word, state] : std::vector<std::pair<std::string, ExecState>>{
           {"stop", ExecState::Stop},
           {"pending", ExecState::Pending},
           {"reboot", ExecState::Reboot},
           {"pending_title", ExecState::PendingTitle},
           {"reboot_title", ExecState::RebootTitle},
           {"later", ExecState::Unknown}}) {
    auto info = rig.mock.info();
    info.execState = word;
    rig.mock.setInfo(info);
    auto got = client.execState();
    REQUIRE_OK(got);
    CHECK(got->state == state);
    CHECK_EQ(got->text, word);
  }
}

XBDM_LINK_TEST(XbdmIntegration, DrivesAndFreeSpace) {
  Rig rig(link);
  auto client = rig.client();
  auto drives = client.drives();
  REQUIRE_OK(drives);
  CHECK_EQ(*drives, (std::vector<std::string>{"HDD", "DEVKIT", "FLASH", "USB0"}));

  auto hdd = client.driveSpace("HDD");
  REQUIRE_OK(hdd);
  CHECK_EQ(hdd->totalBytes, 250000000000ull);
  CHECK_EQ(hdd->freeToCaller, 100000000000ull);
  CHECK_EQ(hdd->totalFreeBytes, 100000000000ull);
  CHECK_EQ(hdd->usedBytes(), 150000000000ull);
  for (const char *form : {"DEVKIT:", "DEVKIT:\\"}) {
    auto d = client.driveSpace(form);
    REQUIRE_OK(d);
    CHECK_EQ(d->totalBytes, 8ull << 30);
  }
  CHECK_EQ(rig.linesNamed("drivefreespace").front(), std::string("drivefreespace name=\"HDD:\\\""));

  auto options = rig.mock.options();
  options.freeSpaceAsSingleLine = true;
  rig.mock.setOptions(options);
  auto single = client.driveSpace("USB0");
  REQUIRE_OK(single);
  CHECK_EQ(single->totalBytes, 16ull << 30);

  auto usb = *rig.mock.drive("USB0");
  usb.refuseFreeSpace = true;
  REQUIRE_OK(rig.mock.updateDrive(usb));
  auto refused = client.driveSpace("USB0");
  CHECK_ERR(refused, ErrorCode::Io);
  CHECK_EQ(statusOf(refused).value_or(0), 414);
  CHECK_EQ(statusOf(client.driveSpace("USB1")).value_or(0), 402);
  CHECK_ERR(client.driveSpace("US B"), ErrorCode::InvalidArgument);
  CHECK(client.isConnected());
  rig.checkCleanTraffic();
}

XBDM_LINK_TEST(XbdmIntegration, Listings) {
  Rig rig(link);
  auto client = rig.client();

  auto root = client.list("HDD:\\");
  REQUIRE_OK(root);
  CHECK_EQ(root->skipped, size_t{0});
  std::vector<std::string> names;
  for (const auto &e : root->entries) names.push_back(e.name);
  CHECK_EQ(names, (std::vector<std::string>{"Content", "default.xex", "Games", "Empty", "Protected", "Attrs"}));
  const auto &content = root->entries[0];
  CHECK(content.isDirectory);
  CHECK_EQ(content.size, 0u);
  CHECK_EQ(content.createdFileTime.value_or(0), ut::filetimeFromUnix(1700000000));
  CHECK_EQ(content.changedFileTime.value_or(0), ut::filetimeFromUnix(1710000000));
  REQUIRE(content.changed().has_value());
  CHECK_EQ(std::chrono::duration_cast<std::chrono::seconds>(content.changed()->time_since_epoch()).count(),
           1710000000ll);
  CHECK(!root->entries[1].isDirectory);
  CHECK_EQ(root->entries[1].size, 20000u);

  auto attrs = client.list("HDD:\\Attrs");
  REQUIRE_OK(attrs);
  REQUIRE_EQ(attrs->entries.size(), size_t{2});
  CHECK(attrs->entries[0].isReadOnly);
  CHECK(!attrs->entries[0].isHidden);
  CHECK(attrs->entries[1].isHidden);

  auto empty = client.list("HDD:\\Empty\\");
  REQUIRE_OK(empty);
  CHECK(empty->entries.empty());

  auto missing = client.list("HDD:\\Nothing");
  CHECK_EQ(statusOf(missing).value_or(0), 402);
  auto file = client.list("HDD:\\default.xex");
  CHECK(statusOf(file).has_value());
  auto denied = client.list("HDD:\\Protected");
  CHECK_EQ(statusOf(denied).value_or(0), 414);
  CHECK(denied.error().message.find("access denied") != std::string::npos);
  CHECK_EQ(statusOf(client.list("USB1:\\")).value_or(0), 402);
  CHECK(client.isConnected());

  for (const auto &line : rig.linesNamed("dirlist")) {
    CHECK_MSG(line.size() >= 3 && line.compare(line.size() - 2, 2, "\\\"") == 0, line);
    CHECK_MSG(line.find("\\\\\"") == std::string::npos, line);
  }
  rig.checkCleanTraffic();
}

XBDM_LINK_TEST(XbdmIntegration, ListingOfManyEntriesAndLargeSizes) {
  Rig rig(link);
  for (int i = 0; i < 300; ++i) REQUIRE_OK(rig.mock.addFile("USB0:\\many\\f" + std::to_string(i) + ".bin", Bytes(size_t(i))));
  REQUIRE_OK(rig.mock.addVirtualFile("USB0:\\huge.bin", (5ull << 30) + 7, 1));
  auto client = rig.client();
  auto many = client.list("USB0:\\many");
  REQUIRE_OK(many);
  REQUIRE_EQ(many->entries.size(), size_t{300});
  CHECK_EQ(many->entries[299].size, 299u);
  auto top = client.list("USB0:\\");
  REQUIRE_OK(top);
  REQUIRE_EQ(top->entries.size(), size_t{2});
  CHECK_EQ(top->entries[1].size, (5ull << 30) + 7);
  auto attributes = client.attributes("USB0:\\huge.bin");
  REQUIRE_OK(attributes);
  CHECK_EQ(attributes->size, (5ull << 30) + 7);
}

XBDM_LINK_TEST(XbdmIntegration, Attributes) {
  Rig rig(link);
  auto client = rig.client();
  auto file = client.attributes("HDD:\\default.xex");
  REQUIRE_OK(file);
  CHECK_EQ(file->size, 20000u);
  CHECK(!file->isDirectory);
  CHECK_EQ(file->createdFileTime.value_or(0), ut::filetimeFromUnix(1700000000));
  auto dir = client.attributes("HDD:\\Content");
  REQUIRE_OK(dir);
  CHECK(dir->isDirectory);
  auto ro = client.attributes("HDD:\\Attrs\\ro.txt");
  REQUIRE_OK(ro);
  CHECK(ro->isReadOnly);
  CHECK_EQ(statusOf(client.attributes("HDD:\\nope")).value_or(0), 402);
  CHECK_EQ(statusOf(client.attributes("HDD:\\Protected\\secret.bin")).value_or(0), 414);

  auto options = rig.mock.options();
  options.attributesAsSingleLine = true;
  rig.mock.setOptions(options);
  auto single = client.attributes("HDD:\\default.xex");
  REQUIRE_OK(single);
  CHECK_EQ(single->size, 20000u);
  CHECK(client.isConnected());
}

XBDM_LINK_TEST(XbdmIntegration, MakeDirectory) {
  Rig rig(link);
  auto client = rig.client();
  REQUIRE_OK(client.makeDirectory("HDD:\\New"));
  REQUIRE(rig.mock.entry("HDD:\\New").has_value());
  CHECK(rig.mock.entry("HDD:\\New")->directory);
  REQUIRE_OK(client.makeDirectory("HDD:\\New\\Inner\\"));
  CHECK(rig.mock.entry("HDD:\\New\\Inner").has_value());

  auto exists = client.makeDirectory("HDD:\\New");
  CHECK_ERR(exists, ErrorCode::Io);
  CHECK_EQ(statusOf(exists).value_or(0), 410);
  CHECK_EQ(statusOf(client.makeDirectory("HDD:\\a\\b\\c")).value_or(0), 413);
  CHECK_EQ(statusOf(client.makeDirectory("FLASH:\\x")).value_or(0), 414);
  CHECK_EQ(statusOf(client.makeDirectory("HDD:\\Protected\\x")).value_or(0), 414);
  CHECK_EQ(statusOf(client.makeDirectory("HDD:\\" + std::string(43, 'n'))).value_or(0), 412);

  rig.mock.clearCommands();
  CHECK_ERR(client.makeDirectory("HDD:\\bad*name"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.makeDirectory("HDD:\\quote\"d"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.makeDirectory("HDD:\\"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.makeDirectory("/HDD/x"), ErrorCode::InvalidArgument);
  CHECK(rig.mock.commands().empty());
  CHECK(client.isConnected());
}

XBDM_LINK_TEST(XbdmIntegration, Remove) {
  Rig rig(link);
  auto client = rig.client();
  REQUIRE_OK(client.removeFile("HDD:\\default.xex"));
  CHECK(!rig.mock.entry("HDD:\\default.xex").has_value());
  REQUIRE_OK(client.removeDirectory("HDD:\\Empty"));
  CHECK(!rig.mock.entry("HDD:\\Empty").has_value());

  CHECK_EQ(statusOf(client.removeDirectory("HDD:\\Content")).value_or(0), 411);
  CHECK_EQ(statusOf(client.removeFile("HDD:\\Content")).value_or(0), 414);
  CHECK_EQ(statusOf(client.removeFile("HDD:\\missing")).value_or(0), 402);
  CHECK_EQ(statusOf(client.removeFile("FLASH:\\kernel.bin")).value_or(0), 414);
  CHECK_EQ(rig.linesNamed("delete")[1], std::string("delete name=\"HDD:\\Empty\" dir"));
  CHECK(client.isConnected());
}

XBDM_LINK_TEST(XbdmIntegration, RenameAndMove) {
  Rig rig(link);
  auto client = rig.client();
  REQUIRE_OK(client.rename("HDD:\\default.xex", "HDD:\\renamed.xex"));
  CHECK(rig.mock.entry("HDD:\\renamed.xex").has_value());
  CHECK(!rig.mock.entry("HDD:\\default.xex").has_value());
  REQUIRE_OK(client.rename("HDD:\\renamed.xex", "HDD:\\Content\\moved.xex"));
  CHECK_EQ(rig.mock.fileData("HDD:\\Content\\moved.xex")->size(), size_t{20000});

  rig.mock.clearCommands();
  CHECK_ERR(client.rename("HDD:\\Content\\moved.xex", "HDD:\\Games"), ErrorCode::InvalidArgument);
  CHECK_EQ(rig.linesNamed("rename").size(), size_t{0});
  CHECK_ERR(client.rename("HDD:\\Games", "DEVKIT:\\Games"), ErrorCode::InvalidArgument);
  CHECK_EQ(statusOf(client.rename("HDD:\\nothing", "HDD:\\other")).value_or(0), 402);
  CHECK_EQ(statusOf(client.rename("FLASH:\\kernel.bin", "FLASH:\\k.bin")).value_or(0), 414);
  CHECK(client.isConnected());
}

XBDM_LINK_TEST(XbdmIntegration, Memory) {
  Rig rig(link);
  auto client = rig.client();
  const auto expected = *rig.mock.memory(0x82000000u, 0x200);

  auto text = client.getMemory(0x82000000u, 0x200);
  REQUIRE_OK(text);
  CHECK_EQ(text->data, expected);
  CHECK_EQ(text->readableBytes(), size_t{0x200});
  auto binary = client.getMemoryEx(0x82000000u, 0x200);
  REQUIRE_OK(binary);
  CHECK_EQ(binary->data, expected);

  auto big = client.getMemoryEx(0x82000000u, 0x10000);
  REQUIRE_OK(big);
  CHECK_EQ(big->data, *rig.mock.memory(0x82000000u, 0x10000));

  auto edge = client.getMemory(0x30000ff8u, 16);
  REQUIRE_OK(edge);
  CHECK_EQ(edge->readableBytes(), size_t{8});
  CHECK(edge->readable[7]);
  CHECK(!edge->readable[8]);
  CHECK_EQ(edge->data[8], 0);
  auto edgeEx = client.getMemoryEx(0x30000ff8u, 16);
  REQUIRE_OK(edgeEx);
  CHECK_EQ(edgeEx->readableBytes(), size_t{8});
  CHECK_EQ(Bytes(edgeEx->data.begin(), edgeEx->data.begin() + 8), Bytes(edge->data.begin(), edge->data.begin() + 8));
  auto none = client.getMemoryEx(0x30001000u, 16);
  REQUIRE_OK(none);
  CHECK_EQ(none->readableBytes(), size_t{0});
  auto unmapped = client.getMemory(0x10000000u, 4);
  REQUIRE_OK(unmapped);
  CHECK_EQ(unmapped->readableBytes(), size_t{0});

  for (const size_t perLine : {size_t{1}, size_t{7}, size_t{128}}) {
    auto options = rig.mock.options();
    options.getmemBytesPerLine = perLine;
    options.lowercaseHex = perLine == 7;
    options.getmemexMarkLastBlock = perLine != 7;
    options.getmemexBlockSize = perLine == 1 ? 1 : 0x400;
    rig.mock.setOptions(options);
    auto again = client.getMemory(0x82000010u, 100);
    REQUIRE_OK(again);
    CHECK_EQ(again->data, Bytes(expected.begin() + 0x10, expected.begin() + 0x10 + 100));
    auto againEx = client.getMemoryEx(0x82000010u, 100);
    REQUIRE_OK(againEx);
    CHECK_EQ(againEx->data, again->data);
  }

  const Bytes patch = ut::patternBytes(100, 5);
  REQUIRE_OK(client.setMemory(0x82000100u, patch));
  CHECK_EQ(*rig.mock.memory(0x82000100u, 100), patch);
  const auto setLines = rig.linesNamed("setmem");
  REQUIRE_EQ(setLines.size(), size_t{2});
  CHECK(setLines[0].rfind("setmem addr=0x82000100 data=", 0) == 0);
  CHECK_EQ(setLines[0].size(), std::string("setmem addr=0x82000100 data=").size() + 128);
  CHECK(setLines[1].rfind("setmem addr=0x82000140 data=", 0) == 0);

  auto refused = client.setMemory(0x10000000u, Bytes{1, 2, 3});
  CHECK_EQ(statusOf(refused).value_or(0), 404);
  CHECK_ERR(client.getMemory(0x82000000u, 0), ErrorCode::InvalidArgument);
  CHECK_ERR(client.getMemoryEx(0xFFFFFFF0u, 0x20), ErrorCode::InvalidArgument);
  CHECK_ERR(client.getMemoryEx(0x82000000u, 0x20001), ErrorCode::LimitExceeded);
  CHECK(client.isConnected());
  rig.checkCleanTraffic();
}

XBDM_LINK_TEST(XbdmIntegration, RegionsModulesAndSections) {
  Rig rig(link);
  auto client = rig.client();
  auto regions = client.memoryRegions();
  REQUIRE_OK(regions);
  REQUIRE_EQ(regions->size(), size_t{2});
  CHECK_EQ((*regions)[0].base, 0x30000000u);
  CHECK_EQ((*regions)[1].size, 0x10000u);
  CHECK_EQ((*regions)[1].protect, 0x4u);

  auto modules = client.modules();
  REQUIRE_OK(modules);
  REQUIRE_EQ(modules->size(), size_t{2});
  CHECK_EQ((*modules)[0].name, std::string("xboxkrnl.exe"));
  CHECK_EQ((*modules)[0].base, 0x80040000u);
  CHECK_EQ((*modules)[0].checksum.value_or(0), 0x00123456u);
  CHECK_EQ((*modules)[0].timestamp.value_or(0), 1600000000u);
  CHECK_EQ((*modules)[1].originalSize.value_or(0), 0x10000u);

  auto sections = client.moduleSections("default.xex");
  REQUIRE_OK(sections);
  REQUIRE_EQ(sections->size(), size_t{3});
  CHECK_EQ((*sections)[1].name, std::string(".text"));
  CHECK_EQ((*sections)[1].index.value_or(9), 1u);
  CHECK_EQ((*sections)[2].flags.value_or(0), 0xc0000040u);
  CHECK_EQ(statusOf(client.moduleSections("nothing.xex")).value_or(0), 402);
  CHECK(client.isConnected());
}

XBDM_LINK_TEST(XbdmIntegration, Screenshot) {
  Rig rig(link);
  auto client = rig.client();
  auto shot = client.screenshot();
  REQUIRE_OK(shot);
  const auto expected = rig.mock.screenshot();
  CHECK_EQ(shot->pitch, expected.pitch);
  CHECK_EQ(shot->width, expected.width);
  CHECK_EQ(shot->height, expected.height);
  CHECK_EQ(shot->format, expected.format);
  CHECK_EQ(shot->data, expected.framebuffer);

  ut::XbdmScreenshot big;
  big.pitch = 0x1400;
  big.width = 0x500;
  big.height = 0x2d0;
  big.offsetX = 3;
  big.offsetY = 4;
  big.framebuffer = ut::patternBytes(size_t{0x1400} * 0x2e0, 9);
  rig.mock.setScreenshot(big);
  auto large = client.screenshot();
  REQUIRE_OK(large);
  CHECK_EQ(large->offsetX, 3u);
  CHECK_EQ(large->offsetY, 4u);
  CHECK_EQ(large->data.size(), big.framebuffer.size());
  CHECK(large->data == big.framebuffer);
  CHECK(client.isConnected());
}

XBDM_LINK_TEST(XbdmIntegration, ClockAndTray) {
  Rig rig(link);
  auto client = rig.client();
  REQUIRE_OK(client.setSystemTimeRaw(0x01d11fb559683c00ull));
  const auto at = updclient::xbdm::fileTimeToTimePoint(ut::filetimeFromUnix(1700000000));
  REQUIRE_OK(client.setSystemTime(*at));
  REQUIRE_OK(client.ejectTray());
  CHECK_EQ(rig.mock.events(), (std::vector<std::string>{"setsystime clock=0x01d11fb559683c00",
                                                         "setsystime clock=0x01da1747c66d0000", "dvdeject"}));
  CHECK_EQ(rig.mock.info().systemTime, ut::filetimeFromUnix(1700000000));
}

XBDM_LINK_TEST(XbdmIntegration, PowerCommands) {
  Rig rig(link);
  auto client = rig.client();

  auto warm = client.reboot(RebootMode::Warm);
  REQUIRE_OK(warm);
  CHECK(*warm == PowerResult::Acknowledged);
  CHECK(!client.isConnected());
  CHECK_ERR(client.debugName(), ErrorCode::NotConnected);
  REQUIRE_OK(client.reconnect());

  auto cold = client.reboot(RebootMode::Cold);
  REQUIRE_OK(cold);
  CHECK(*cold == PowerResult::ConnectionClosed);
  REQUIRE_OK(client.reconnect());

  auto options = rig.mock.options();
  options.coldRebootAnswersFirst = true;
  rig.mock.setOptions(options);
  auto coldAnswered = client.reboot(RebootMode::Cold);
  REQUIRE_OK(coldAnswered);
  CHECK(*coldAnswered == PowerResult::Acknowledged);
  REQUIRE_OK(client.reconnect());

  auto launched = client.launch("HDD:\\Games\\Mock\\default.xex");
  REQUIRE_OK(launched);
  CHECK(*launched == PowerResult::Acknowledged);
  REQUIRE_OK(client.reconnect());
  auto rootLaunch = client.launch("FLASH:\\kernel.bin");
  REQUIRE_OK(rootLaunch);
  REQUIRE_OK(client.reconnect());

  auto off = client.shutdown();
  REQUIRE_OK(off);
  CHECK(*off == PowerResult::ConnectionClosed);

  CHECK_EQ(rig.mock.events(),
           (std::vector<std::string>{"magicboot", "magicboot cold", "magicboot cold",
                                     "magicboot title=HDD:\\Games\\Mock\\default.xex directory=HDD:\\Games\\Mock",
                                     "magicboot title=FLASH:\\kernel.bin directory=FLASH:\\", "shutdown"}));
  CHECK_EQ(rig.linesNamed("magicboot")[1], std::string("magicboot cold"));
  rig.checkCleanTraffic();
}

XBDM_LINK_TEST(XbdmIntegration, RebootDropsTheOtherConnectionsToo) {
  Rig rig(link);
  auto first = rig.client();
  auto second = rig.client();
  REQUIRE_OK(first.reboot());
  REQUIRE(rig.mock.waitForActiveConnections(0));
  auto stale = second.debugName();
  CHECK_ERR(stale, ErrorCode::Disconnected);
  CHECK(!second.isConnected());
  REQUIRE_OK(second.reconnect());
  CHECK_OK(second.debugName());
}

XBDM_LINK_TEST(XbdmIntegration, ByeOnClose) {
  Rig rig(link);
  {
    auto client = rig.client();
    REQUIRE_OK(client.debugName());
  }
  REQUIRE(rig.mock.waitForActiveConnections(0));
  CHECK_EQ(rig.mock.commandLines(), (std::vector<std::string>{"dbgname", "bye"}));
}

XBDM_LINK_TEST(XbdmIntegration, ConnectionLimit) {
  Rig rig(link);
  std::vector<XbdmClient> clients;
  for (int i = 0; i < 4; ++i) clients.push_back(rig.client());
  auto fifth = rig.open();
  CHECK_ERR(fifth, ErrorCode::LimitExceeded);
  if (!fifth) {
    CHECK_EQ(consoleStatusCode(fifth.error()).value_or(0), 401);
    CHECK(fifth.error().message.find("max number of connections") != std::string::npos);
  }
  CHECK_EQ(rig.mock.connectionsRefused(), size_t{1});
  for (auto &c : clients) CHECK_OK(c.debugName());

  clients.pop_back();
  REQUIRE(rig.mock.waitForActiveConnections(3));
  auto again = rig.open();
  REQUIRE_OK(again);
  CHECK_OK(again->debugName());
}

XBDM_LINK_TEST(XbdmIntegration, ParallelClients) {
  Rig rig(link);
  for (int i = 0; i < 3; ++i) REQUIRE_OK(rig.mock.addFile("HDD:\\p" + std::to_string(i) + ".bin", ut::patternBytes(200000, 20 + i)));
  std::atomic<int> good{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 3; ++i) {
    threads.emplace_back([&, i] {
      auto client = rig.open();
      if (!client) return;
      for (int round = 0; round < 3; ++round) {
        auto reader = client->openRead("HDD:\\p" + std::to_string(i) + ".bin");
        if (!reader) return;
        Bytes all;
        Bytes chunk(7777);
        while (true) {
          auto n = reader->read(chunk);
          if (!n) return;
          if (*n == 0) break;
          all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*n));
        }
        if (all != ut::patternBytes(200000, 20 + i)) return;
        if (!client->list("HDD:\\")) return;
      }
      ++good;
    });
  }
  for (auto &t : threads) t.join();
  CHECK_EQ(good.load(), 3);
  rig.checkCleanTraffic();
}

XBDM_LINK_TEST(XbdmIntegration, CommandLineLimits) {
  Rig rig(link);
  auto client = rig.client();
  std::string path = "HDD:";
  while (path.size() < 560) path += "\\abcdefghijklmnopqrstuvwxyz0123456789abcd";
  auto tooLongForMock = client.list(path);
  CHECK_ERR(tooLongForMock, ErrorCode::LimitExceeded);
  CHECK_EQ(statusOf(tooLongForMock).value_or(0), 406);
  CHECK(client.isConnected());
  while (path.size() < 1100) path += "\\abcdefghijklmnopqrstuvwxyz0123456789abcd";
  rig.mock.clearCommands();
  CHECK_ERR(client.list(path), ErrorCode::LimitExceeded);
  CHECK(rig.mock.commands().empty());
  CHECK_OK(client.debugName());
}

// Every refusal in the spec's table, on commands of each answer shape, keeps the
// connection; codes other than 2xx and 4xx drop it.
XBDM_LINK_TEST(XbdmIntegration, EveryRefusalCodeKeepsTheConnection) {
  Rig rig(link);
  auto client = rig.client();
  const int codes[] = {400, 401, 402, 403, 404, 405, 406, 407, 408, 409, 410, 411, 412, 413, 414, 415, 416,
                       417, 420, 421, 422, 423, 426, 428, 430, 437, 446, 480, 496, 497};
  for (const int code : codes) {
    const std::string line = std::to_string(code) + "- refused for the test";
    rig.mock.inject(XbdmFault::statusLine(line).on("dbgname"));
    auto single = client.debugName();
    REQUIRE(!single);
    CHECK_EQ(consoleStatusCode(single.error()).value_or(0), code);
    const ErrorCode expected = (code == 401 || code == 406 || code == 446) ? ErrorCode::LimitExceeded
                               : code == 407                            ? ErrorCode::Unsupported
                                                                        : ErrorCode::Io;
    CHECK_EQ(single.error().code, expected);
    rig.mock.inject(XbdmFault::statusLine(line).on("dirlist"));
    CHECK_EQ(statusOf(client.list("HDD:\\")).value_or(0), code);
    rig.mock.inject(XbdmFault::statusLine(line).on("getfile"));
    CHECK_EQ(statusOf(client.openRead("HDD:\\default.xex")).value_or(0), code);
    rig.mock.inject(XbdmFault::statusLine(line).on("getmemex"));
    CHECK_EQ(statusOf(client.getMemoryEx(0x82000000u, 4)).value_or(0), code);
    rig.mock.inject(XbdmFault::statusLine(line).on("magicboot"));
    CHECK_EQ(statusOf(client.reboot()).value_or(0), code);
    REQUIRE(client.isConnected());
  }
  CHECK_EQ(client.lastStatus()->code, 497);
  CHECK_OK(client.debugName());
}
