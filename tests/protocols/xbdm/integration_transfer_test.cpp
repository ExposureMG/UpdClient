#include "protocols/xbdm/integration_support.hpp"

#include <updclient/core/path.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace xit;
using updclient::xbdm::consoleStatusCode;
using updclient::xbdm::FileReader;
using updclient::xbdm::FileWriter;

namespace {

// Around every piece size in the path: the client's 4 KiB line reads and 64 KiB
// transfer pieces, the mock's 64 KiB pieces and the pipe's 64 KiB capacity.
const std::vector<size_t> kSizes = {0,     1,     2,     3,     4095,  4096,   4097,   65535,
                                    65536, 65537, 131071, 131072, 131075, 196608, 1048577};

Bytes readAll(FileReader &reader, size_t piece) {
  Bytes all;
  Bytes chunk(piece);
  while (true) {
    auto n = reader.read(chunk);
    if (!n) {
      ut::recordFailure(__FILE__, __LINE__, "read", updclient::formatError(n.error()));
      return all;
    }
    if (*n == 0) return all;
    all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*n));
  }
}

// ut::readFile goes byte by byte, which an unoptimised build feels at 64 MiB.
std::optional<Bytes> readLarge(const std::filesystem::path &path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return std::nullopt;
  std::ifstream in(path, std::ios::binary);
  Bytes data(static_cast<size_t>(size));
  in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
  if (static_cast<uint64_t>(in.gcount()) != size) return std::nullopt;
  return data;
}

std::string sized(size_t n) { return "HDD:\\Content\\s" + std::to_string(n) + ".bin"; }

} // namespace

XBDM_LINK_TEST(XbdmTransferIntegration, DownloadSizes) {
  Rig rig(link);
  ut::TempDir dir;
  REQUIRE(dir.ok());
  for (size_t n : kSizes) REQUIRE_OK(rig.mock.addFile(sized(n), ut::patternBytes(n, static_cast<uint32_t>(n))));
  auto client = rig.client();
  for (size_t n : kSizes) {
    const auto local = dir.file("d" + std::to_string(n));
    REQUIRE_OK(client.downloadToFile(sized(n), local));
    CHECK_EQ(ut::readFile(local).value_or(Bytes{9}), ut::patternBytes(n, static_cast<uint32_t>(n)));

    for (size_t piece : {size_t{1}, size_t{1000}, size_t{65536}, size_t{70000}}) {
      if (piece == 1 && n > 70000) continue;
      auto reader = client.openRead(sized(n), n);
      REQUIRE_OK(reader);
      CHECK_EQ(reader->size(), n);
      CHECK_EQ(reader->isOpen(), n > 0);
      CHECK_EQ(client.transferActive(), n > 0);
      CHECK_EQ(readAll(*reader, piece), ut::patternBytes(n, static_cast<uint32_t>(n)));
      CHECK(!client.transferActive());
    }
  }
  CHECK_EQ(dir.entries().size(), kSizes.size());
  CHECK_OK(client.debugName());
  rig.checkCleanTraffic();
}

XBDM_LINK_TEST(XbdmTransferIntegration, UploadSizes) {
  Rig rig(link);
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto client = rig.client();
  for (size_t n : kSizes) {
    const auto local = dir.file("u" + std::to_string(n));
    const Bytes data = ut::patternBytes(n, static_cast<uint32_t>(n + 1));
    REQUIRE(ut::writeFile(local, data));
    REQUIRE_OK(client.uploadFromFile(local, sized(n)));
    CHECK_EQ(rig.mock.fileData(sized(n)).value_or(Bytes{9}), data);

    const std::string again = "HDD:\\Content\\w" + std::to_string(n) + ".bin";
    auto writer = client.openWrite(again, n);
    REQUIRE_OK(writer);
    CHECK(writer->temporaryPath().rfind("HDD:\\Content\\w" + std::to_string(n) + ".bin.", 0) == 0);
    size_t at = 0;
    for (size_t piece = 1; at < n; piece = piece * 3 + 1) {
      const size_t take = std::min(piece, n - at);
      REQUIRE_OK(writer->write(std::span<const uint8_t>(data).subspan(at, take)));
      at += take;
    }
    CHECK_ERR(writer->write(Bytes{1}), ErrorCode::InvalidArgument);
    REQUIRE_OK(writer->finish());
    CHECK_EQ(rig.mock.fileData(again).value_or(Bytes{9}), data);
  }
  CHECK(partFiles(rig.mock, "HDD:\\Content").empty());
  for (const auto &u : rig.mock.uploads()) {
    CHECK(u.completed);
    CHECK_EQ(u.announced, u.received);
    CHECK(u.path.size() > 5 && u.path.compare(u.path.size() - 5, 5, ".part") == 0);
  }
  for (const auto &line : rig.linesNamed("sendfile")) CHECK_MSG(line.find(" length=0x") != std::string::npos, line);
  CHECK_OK(client.debugName());
  rig.checkCleanTraffic();
}

XBDM_LINK_TEST(XbdmTransferIntegration, UploadReplacesAnExistingFileOnlyAfterTheData) {
  Rig rig(link);
  ut::TempDir dir;
  const auto local = dir.file("new.bin");
  REQUIRE(ut::writeFile(local, ut::bytesOf("new contents")));
  auto client = rig.client();
  REQUIRE_OK(client.uploadFromFile(local, "HDD:\\default.xex"));
  CHECK_EQ(*rig.mock.fileData("HDD:\\default.xex"), ut::bytesOf("new contents"));
  CHECK(partFiles(rig.mock, "HDD:\\").empty());

  const auto lines = rig.mock.commandLines();
  std::vector<std::string> names;
  for (const auto &record : rig.mock.commands()) names.push_back(record.name);
  CHECK_EQ(names, (std::vector<std::string>{"sendfile", "getfileattributes", "delete", "rename"}));

  auto onFolder = client.uploadFromFile(local, "HDD:\\Content");
  CHECK_ERR(onFolder, ErrorCode::InvalidArgument);
  CHECK(partFiles(rig.mock, "HDD:\\").empty());
  CHECK(rig.mock.entry("HDD:\\Content")->directory);

  CHECK_EQ(consoleStatusCode(client.uploadFromFile(local, "HDD:\\no\\such\\file.bin").error()).value_or(0), 413);
  CHECK_EQ(consoleStatusCode(client.uploadFromFile(local, "FLASH:\\new.bin").error()).value_or(0), 414);
  CHECK_OK(client.debugName());
}

XBDM_LINK_TEST(XbdmTransferIntegration, LargeTransfers) {
  Rig rig(link);
  ut::TempDir dir;
  const uint64_t size = 64ull << 20;
  REQUIRE_OK(rig.mock.addVirtualFile("USB0:\\big.bin", size, 77));
  auto client = rig.client();
  const auto local = dir.file("big.bin");
  uint64_t lastDone = 0;
  int calls = 0;
  REQUIRE_OK(client.downloadToFile("USB0:\\big.bin", local, [&](uint64_t done, uint64_t total) {
    CHECK_EQ(total, size);
    CHECK(done >= lastDone);
    lastDone = done;
    ++calls;
  }));
  CHECK_EQ(lastDone, size);
  CHECK(calls > 2);
  const auto got = readLarge(local);
  REQUIRE(got.has_value());
  REQUIRE_EQ(got->size(), size_t{size});
  Bytes expected(size);
  ut::virtualFileBytes(77, 0, expected);
  CHECK(*got == expected);

  REQUIRE_OK(client.uploadFromFile(local, "USB0:\\copy.bin"));
  CHECK(rig.mock.fileData("USB0:\\copy.bin").value_or(Bytes{}) == expected);
  CHECK(partFiles(rig.mock, "USB0:\\").empty());
}

XBDM_LINK_TEST(XbdmTransferIntegration, ManySmallFilesOnOneConnection) {
  Rig rig(link);
  ut::TempDir dir;
  auto client = rig.client();
  REQUIRE_OK(client.makeDirectory("HDD:\\many"));
  for (int i = 0; i < 120; ++i) {
    const auto local = dir.file("m" + std::to_string(i));
    REQUIRE(ut::writeFile(local, ut::patternBytes(static_cast<size_t>(i * 37 % 1500), static_cast<uint32_t>(i))));
    REQUIRE_OK(client.uploadFromFile(local, "HDD:\\many\\f" + std::to_string(i) + ".dat"));
  }
  auto listing = client.list("HDD:\\many");
  REQUIRE_OK(listing);
  CHECK_EQ(listing->entries.size(), size_t{120});
  for (int i = 0; i < 120; ++i) {
    const auto back = dir.file("b" + std::to_string(i));
    REQUIRE_OK(client.downloadToFile("HDD:\\many\\f" + std::to_string(i) + ".dat", back));
    CHECK_EQ(*ut::readFile(back), ut::patternBytes(static_cast<size_t>(i * 37 % 1500), static_cast<uint32_t>(i)));
  }
  CHECK_EQ(rig.mock.connectionsAccepted(), size_t{1});
  rig.checkCleanTraffic();
}

XBDM_LINK_TEST(XbdmTransferIntegration, OneTransferOwnsTheConnection) {
  Rig rig(link);
  auto client = rig.client();
  auto reader = client.openRead("HDD:\\default.xex");
  REQUIRE_OK(reader);
  CHECK_ERR(client.debugName(), ErrorCode::InvalidArgument);
  CHECK_ERR(client.openRead("HDD:\\default.xex"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.reconnect(), ErrorCode::InvalidArgument);
  Bytes part(100);
  REQUIRE_OK(reader->read(part));
  reader->close();
  CHECK(!client.isConnected());
  REQUIRE_OK(client.reconnect());
  CHECK_OK(client.debugName());

  auto writer = client.openWrite("HDD:\\w.bin", 10);
  REQUIRE_OK(writer);
  CHECK_ERR(client.list("HDD:\\"), ErrorCode::InvalidArgument);
  CHECK_ERR(writer->finish(), ErrorCode::InvalidArgument);
  REQUIRE_OK(writer->write(Bytes(10, 1)));
  REQUIRE_OK(writer->finish());
  CHECK_EQ(*rig.mock.fileData("HDD:\\w.bin"), Bytes(10, 1));
}

// Files of 4 GiB and more: getfile's length has 32 bits (section 3.5) and sendfile
// beyond 4 GiB - 1 is unknown (section 3.6). The mock serves a virtual file.
XBDM_LINK_TEST(XbdmTransferIntegration, AboveFourGiB) {
  Rig rig(link);
  ut::TempDir dir;
  const uint64_t huge = (4ull << 30) + 4096;
  REQUIRE_OK(rig.mock.addVirtualFile("USB0:\\huge.bin", huge, 3));
  auto client = rig.client();
  auto listed = client.attributes("USB0:\\huge.bin");
  REQUIRE_OK(listed);
  CHECK_EQ(listed->size, huge);

  rig.mock.clearCommands();
  CHECK_ERR(client.openRead("USB0:\\huge.bin", listed->size), ErrorCode::Unsupported);
  CHECK(rig.linesNamed("getfile").empty());

  const auto local = dir.file("huge.bin");
  auto download = client.downloadToFile("USB0:\\huge.bin", local);
  CHECK_ERR(download, ErrorCode::Unsupported);
  CHECK(rig.linesNamed("getfile").empty());
  CHECK(dir.entries().empty());
  CHECK(client.isConnected());

  CHECK_ERR(client.openWrite("USB0:\\up.bin", huge), ErrorCode::LimitExceeded);
  auto options = client.options();
  options.maxUploadBytes = huge;
  client.setOptions(options);
  auto refused = client.openWrite("USB0:\\up.bin", huge);
  REQUIRE(!refused);
  CHECK_EQ(consoleStatusCode(refused.error()).value_or(0), 415);
  REQUIRE_EQ(rig.linesNamed("sendfile").size(), size_t{1});
  CHECK(rig.linesNamed("sendfile")[0].find(" length=0x100001000") != std::string::npos);
  CHECK(partFiles(rig.mock, "USB0:\\").empty());
  CHECK_OK(client.debugName());
}

// Real data past 4 GiB, over loopback TCP only and only on request: it moves
// 4 GiB + 1 in each direction (UPDCLIENT_XBDM_HUGE=1).
TEST(XbdmTransferIntegration, HugeTransfersOnRequest) {
  const char *flag = std::getenv("UPDCLIENT_XBDM_HUGE");
  if (!flag || std::string(flag) != "1") SKIP("set UPDCLIENT_XBDM_HUGE=1 to move 4 GiB + 1 in each direction");
  XbdmMockOptions mockOptions;
  mockOptions.maxUploadBytes = 8ull << 30;
  mockOptions.storeUploads = false;
  Rig rig(Link::Tcp, mockOptions);
  auto o = quickOptions();
  o.maxUploadBytes = 8ull << 30;
  auto client = rig.client(o);

  const uint64_t below = 0xFFFFFFFFull;
  REQUIRE_OK(rig.mock.addVirtualFile("USB0:\\max.bin", below, 5));
  auto reader = client.openRead("USB0:\\max.bin", below);
  REQUIRE_OK(reader);
  ut::Fnv1a digest;
  Bytes chunk(1 << 20);
  uint64_t total = 0;
  while (true) {
    auto n = reader->read(chunk);
    REQUIRE_OK(n);
    if (*n == 0) break;
    digest.add(std::span<const uint8_t>(chunk).first(*n));
    total += *n;
  }
  CHECK_EQ(total, below);
  CHECK_EQ(digest.value, *rig.mock.fileDigest("USB0:\\max.bin"));

  ut::TempDir dir;
  const auto sparse = dir.file("sparse.bin");
  {
    std::ofstream out(sparse, std::ios::binary);
    out.seekp(static_cast<std::streamoff>(4ull << 30));
    out.put('\x5a');
  }
  REQUIRE_EQ(std::filesystem::file_size(sparse), (4ull << 30) + 1);
  REQUIRE_OK(client.uploadFromFile(sparse, "USB0:\\over.bin"));
  CHECK_EQ(*rig.mock.fileSize("USB0:\\over.bin"), (4ull << 30) + 1);
  CHECK(rig.linesNamed("sendfile")[0].find(" length=0x100000001") != std::string::npos);
}

XBDM_LINK_TEST(XbdmTransferIntegration, CancelABlockedDownload) {
  Rig rig(link);
  ut::TempDir dir;
  REQUIRE_OK(rig.mock.addFile("HDD:\\slow.bin", ut::patternBytes(500000, 1)));
  rig.mock.inject(XbdmFault::stall(30 + 4 + 200000).on("getfile").repeat(3));
  auto o = quickOptions();
  o.idleTimeout = 30000ms;
  auto client = rig.client(o);

  const auto local = dir.file("slow.bin");
  std::atomic<bool> done{false};
  updclient::Result<void> result;
  std::thread worker([&] {
    result = client.downloadToFile("HDD:\\slow.bin", local);
    done = true;
  });
  for (int i = 0; i < 2500 && rig.linesNamed("getfile").empty(); ++i) std::this_thread::sleep_for(2ms);
  std::this_thread::sleep_for(100ms);
  CHECK(!done);
  const auto start = std::chrono::steady_clock::now();
  client.cancel();
  worker.join();
  CHECK(std::chrono::steady_clock::now() - start < 5s);
  CHECK_ERR(result, ErrorCode::Cancelled);
  CHECK(dir.entries().empty());
  CHECK(!client.isConnected());
  REQUIRE_OK(client.reconnect());

  auto reader = client.openRead("HDD:\\slow.bin");
  REQUIRE_OK(reader);
  updclient::Result<size_t> last = size_t{0};
  std::thread readerThread([&] {
    Bytes chunk(65536);
    while (true) {
      last = reader->read(chunk);
      if (!last || *last == 0) break;
    }
  });
  std::this_thread::sleep_for(200ms);
  reader->cancel();
  readerThread.join();
  CHECK_ERR(last, ErrorCode::Cancelled);
  CHECK(!reader->isOpen());
  CHECK(!client.transferActive());
  REQUIRE_OK(client.reconnect());

  auto third = client.openRead("HDD:\\slow.bin");
  REQUIRE_OK(third);
  Bytes chunk(1000);
  REQUIRE_OK(third->read(chunk));
  client.close();
  CHECK(!client.isConnected());
  auto afterClose = third->read(chunk);
  CHECK(!afterClose);
  if (!afterClose) {
    CHECK(afterClose.error().code == ErrorCode::Cancelled || afterClose.error().code == ErrorCode::NotConnected);
  }
}

XBDM_LINK_TEST(XbdmTransferIntegration, CancelABlockedUpload) {
  Rig rig(link);
  rig.mock.inject(XbdmFault::stallUploadAfterBytes(1000).on("sendfile"));
  auto client = rig.client();
  const size_t size = 32u << 20;
  auto writer = client.openWrite("HDD:\\blocked.bin", size);
  REQUIRE_OK(writer);
  const std::string temp = writer->temporaryPath();
  updclient::Result<void> result;
  std::thread worker([&] {
    const Bytes piece(1 << 20, 0x42);
    for (size_t at = 0; at < size; at += piece.size()) {
      result = writer->write(piece);
      if (!result) return;
    }
    result = writer->finish();
  });
  std::this_thread::sleep_for(300ms);
  const auto start = std::chrono::steady_clock::now();
  writer->cancel();
  worker.join();
  CHECK(std::chrono::steady_clock::now() - start < 5s);
  CHECK_ERR(result, ErrorCode::Cancelled);
  CHECK(!writer->isOpen());
  CHECK_EQ(client.pendingCleanup(), (std::vector<std::string>{temp}));
  CHECK(!rig.mock.entry("HDD:\\blocked.bin").has_value());

  rig.mock.dropAllConnections();
  REQUIRE(rig.mock.waitForActiveConnections(0));
  REQUIRE_OK(client.reconnect());
  CHECK(client.pendingCleanup().empty());
  CHECK(partFiles(rig.mock, "HDD:\\").empty());
  CHECK(!rig.mock.entry("HDD:\\blocked.bin").has_value());
}

XBDM_LINK_TEST(XbdmTransferIntegration, CancelWhileWaitingForTheUploadStatus) {
  Rig rig(link);
  ut::TempDir dir;
  const auto local = dir.file("f.bin");
  REQUIRE(ut::writeFile(local, ut::patternBytes(5000, 2)));
  rig.mock.inject(XbdmFault::stall(23).on("sendfile"));
  auto o = quickOptions();
  o.slowIdleTimeout = 30000ms;
  auto client = rig.client(o);
  updclient::Result<void> result;
  std::thread worker([&] { result = client.uploadFromFile(local, "HDD:\\f.bin"); });
  std::this_thread::sleep_for(200ms);
  client.cancel();
  worker.join();
  CHECK_ERR(result, ErrorCode::Cancelled);
  CHECK(!rig.mock.entry("HDD:\\f.bin").has_value());
  REQUIRE_EQ(client.pendingCleanup().size(), size_t{1});
  CHECK_EQ(partFiles(rig.mock, "HDD:\\").size(), size_t{1});
  REQUIRE_OK(client.reconnect());
  CHECK(partFiles(rig.mock, "HDD:\\").empty());
}

// A drop after every byte of the upload data: the final name is never created and
// the temporary file the mock kept is deleted by the next reconnect().
XBDM_LINK_TEST(XbdmTransferIntegration, DropAtEveryUploadOffset) {
  Rig rig(link);
  ut::TempDir dir;
  const size_t size = link == Link::Memory ? 300 : 120;
  const Bytes data = ut::patternBytes(size, 4);
  const auto local = dir.file("up.bin");
  REQUIRE(ut::writeFile(local, data));
  REQUIRE_OK(rig.mock.addFile("HDD:\\keep.bin", ut::bytesOf("old")));
  auto client = rig.client();
  for (size_t k = 0; k <= size; ++k) {
    rig.mock.inject(XbdmFault::dropUploadAfterBytes(k).on("sendfile"));
    auto r = client.uploadFromFile(local, "HDD:\\keep.bin");
    if (k == size) {
      CHECK_OK(r);
      CHECK_EQ(*rig.mock.fileData("HDD:\\keep.bin"), data);
      continue;
    }
    CHECK_MSG(!r, "offset " + std::to_string(k));
    CHECK_EQ(*rig.mock.fileData("HDD:\\keep.bin"), ut::bytesOf("old"));
    CHECK_EQ(client.pendingCleanup().size(), size_t{1});
    REQUIRE(rig.mock.waitForActiveConnections(0));
    const auto parts = partFiles(rig.mock, "HDD:\\");
    CHECK_EQ(parts.size(), size_t{1});
    if (parts.size() == 1) CHECK_EQ(rig.mock.fileSize("HDD:\\" + parts[0]).value_or(999), uint64_t{k});
    REQUIRE_OK(client.reconnect());
    CHECK(client.pendingCleanup().empty());
    CHECK(partFiles(rig.mock, "HDD:\\").empty());
  }
}

// Drops inside the sendfile answers: in the 204 line, and after all the data but
// before the status.
XBDM_LINK_TEST(XbdmTransferIntegration, DropAroundTheUploadData) {
  Rig rig(link);
  ut::TempDir dir;
  const auto local = dir.file("up.bin");
  REQUIRE(ut::writeFile(local, ut::patternBytes(64, 4)));
  auto client = rig.client();
  const size_t line204 = std::string("204- send binary data\r\n").size();
  for (size_t k = 0; k <= line204 + 5; ++k) {
    rig.mock.inject(XbdmFault::dropAfterBytes(k).on("sendfile"));
    auto r = client.uploadFromFile(local, "HDD:\\x.bin");
    CHECK_MSG(!r, "offset " + std::to_string(k));
    CHECK(!rig.mock.entry("HDD:\\x.bin").has_value());
    REQUIRE(rig.mock.waitForActiveConnections(0));
    REQUIRE_OK(client.reconnect());
    CHECK_MSG(partFiles(rig.mock, "HDD:\\").empty(), "offset " + std::to_string(k));
  }
  rig.mock.inject(XbdmFault::uploadAnswer("415- no room on device").on("sendfile"));
  auto full = client.uploadFromFile(local, "HDD:\\x.bin");
  CHECK_EQ(consoleStatusCode(full.error()).value_or(0), 415);
  CHECK(client.isConnected());
  CHECK(partFiles(rig.mock, "HDD:\\").empty());
  CHECK(!rig.mock.entry("HDD:\\x.bin").has_value());
}

// A drop after every byte of the getfile answer: nothing is left on the host
// unless the whole file arrived.
XBDM_LINK_TEST(XbdmTransferIntegration, DropAtEveryDownloadOffset) {
  Rig rig(link);
  ut::TempDir dir;
  const size_t size = link == Link::Memory ? 300 : 120;
  REQUIRE_OK(rig.mock.addFile("HDD:\\d.bin", ut::patternBytes(size, 6)));
  auto client = rig.client();
  const size_t total = std::string("203- binary response follows\r\n").size() + 4 + size;
  const auto local = dir.file("d.bin");
  for (size_t k = 0; k <= total; ++k) {
    rig.mock.inject(XbdmFault::dropAfterBytes(k).on("getfile"));
    auto r = client.downloadToFile("HDD:\\d.bin", local);
    if (k == total) {
      CHECK_OK(r);
      CHECK_EQ(*ut::readFile(local), ut::patternBytes(size, 6));
      break;
    }
    CHECK_MSG(!r, "offset " + std::to_string(k));
    if (!r) CHECK_MSG(r.error().code == ErrorCode::Disconnected, updclient::formatError(r.error()));
    CHECK_MSG(dir.entries().empty(), "offset " + std::to_string(k));
    REQUIRE_OK(client.reconnect());
  }
}

XBDM_LINK_TEST(XbdmTransferIntegration, DownloadOfAFolderOrMissingFile) {
  Rig rig(link);
  ut::TempDir dir;
  auto client = rig.client();
  const auto local = dir.file("x");
  CHECK_EQ(consoleStatusCode(client.downloadToFile("HDD:\\Content", local).error()).value_or(0), 414);
  CHECK_EQ(consoleStatusCode(client.downloadToFile("HDD:\\missing", local).error()).value_or(0), 402);
  CHECK_EQ(consoleStatusCode(client.downloadToFile("HDD:\\Protected\\secret.bin", local).error()).value_or(0), 414);
  CHECK(dir.entries().empty());
  CHECK(client.isConnected());
}

XBDM_LINK_TEST(XbdmTransferIntegration, AwkwardNames) {
  Rig rig(link);
  ut::TempDir dir;
  const Bytes data = ut::patternBytes(10, 1);
  REQUIRE(ut::writeFile(dir.file("a"), data));
  auto client = rig.client();
  for (const std::string &name : {std::string(" lead and trail "), std::string("dots.in.the.middle."),
                                  std::string("#$%&'()+,;=@[]^_`{}~"), std::string(42, 'L')}) {
    const std::string path = "HDD:\\Content\\" + name;
    REQUIRE_OK(client.uploadFromFile(dir.file("a"), path));
    CHECK_EQ(rig.mock.fileData(path).value_or(Bytes{}), data);
    auto listing = client.list("HDD:\\Content");
    REQUIRE_OK(listing);
    CHECK(std::any_of(listing->entries.begin(), listing->entries.end(), [&](const auto &e) { return e.name == name; }));
    REQUIRE_OK(client.downloadToFile(path, dir.file("b")));
    CHECK_EQ(*ut::readFile(dir.file("b")), data);
    const std::string moved = "HDD:\\Content\\" + name.substr(1) + "~";
    REQUIRE_OK(client.rename(path, moved));
    REQUIRE_OK(client.removeFile(moved));
  }
  for (const auto &u : rig.mock.uploads()) {
    CHECK_MSG(u.path.size() - u.path.rfind('\\') - 1 <= 42, u.path);
  }
  auto tooLong = client.uploadFromFile(dir.file("a"), "HDD:\\Content\\" + std::string(43, 'L'));
  CHECK_EQ(consoleStatusCode(tooLong.error()).value_or(0), 412);
  CHECK(partFiles(rig.mock, "HDD:\\Content").empty());
  CHECK(client.isConnected());
}

XBDM_LINK_TEST(XbdmTransferIntegration, TraceSeesEveryLineButNoFileData) {
  Rig rig(link);
  ut::TempDir dir;
  const std::string marker = "SECRET-FILE-CONTENTS-";
  Bytes secret;
  for (int i = 0; i < 2000; ++i) ut::append(secret, marker);
  REQUIRE_OK(rig.mock.addFile("HDD:\\secret.bin", secret));
  REQUIRE(ut::writeFile(dir.file("up"), secret));

  struct Event {
    updclient::xbdm::TraceEvent kind;
    std::string text;
    uint64_t bytes;
  };
  std::vector<Event> events;
  auto options = quickOptions();
  options.trace = [&](updclient::xbdm::TraceEvent kind, std::string_view text, uint64_t bytes) {
    events.push_back({kind, std::string(text), bytes});
  };
  {
    auto client = rig.client(options);
    REQUIRE_OK(client.downloadToFile("HDD:\\secret.bin", dir.file("down")));
    REQUIRE_OK(client.uploadFromFile(dir.file("up"), "HDD:\\copy.bin"));
    REQUIRE_OK(client.list("HDD:\\"));
  }
  using updclient::xbdm::TraceEvent;
  std::vector<std::string> sent;
  uint64_t binaryIn = 0, binaryOut = 0;
  for (const auto &e : events) {
    CHECK_MSG(e.text.find(marker) == std::string::npos, "file data in the trace: " + e.text.substr(0, 40));
    if (e.kind == TraceEvent::Sent) sent.push_back(e.text);
    if (e.kind == TraceEvent::BinaryReceived) binaryIn += e.bytes;
    if (e.kind == TraceEvent::BinarySent) binaryOut += e.bytes;
  }
  CHECK_EQ(sent, rig.mock.commandLines());
  CHECK_EQ(binaryIn, uint64_t{4} + secret.size());
  CHECK_EQ(binaryOut, uint64_t{secret.size()});
  REQUIRE(!events.empty());
  CHECK(events.front().kind == TraceEvent::Received);
  CHECK_EQ(events.front().text, std::string("201- connected"));
  CHECK(std::any_of(events.begin(), events.end(),
                    [](const Event &e) { return e.text == "202- multiline response follows"; }));
  CHECK(std::any_of(events.begin(), events.end(), [](const Event &e) { return e.text.rfind("name=\"Content\"", 0) == 0; }));
}
