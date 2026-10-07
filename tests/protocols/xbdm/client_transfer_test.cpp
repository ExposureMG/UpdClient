#include "protocols/xbdm/client_fake.hpp"
#include "support/loopback_server.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/xbdm/client.hpp>
#include <protocols/xbdm/protocol.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace updclient;
using namespace updclient::xbdm;
using xt::FakeConsole;

namespace {

XbdmClient connected(const std::shared_ptr<FakeConsole> &console, ClientOptions options = xt::quickOptions(),
                     XbdmClient::Connector connector = {}) {
  auto client = xt::attach(console, options, std::move(connector));
  if (!client) throw std::runtime_error("attach failed: " + formatError(client.error()));
  return std::move(*client);
}

ut::Bytes le32(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

std::string quotedArg(const std::string &line, const std::string &key) {
  const std::string marker = key + "=\"";
  const size_t start = line.find(marker);
  if (start == std::string::npos) return {};
  const size_t end = line.find('"', start + marker.size());
  return line.substr(start + marker.size(), end - start - marker.size());
}

// A tiny console file system: enough of sendfile, getfileattributes, delete and
// rename to follow an upload through.
struct Files {
  std::map<std::string, ut::Bytes> files;
  std::vector<std::string> folders;
  int afterDataStatus = 200;
  int renameStatus = 200;
  int sendfileStatus = 204;

  void install(FakeConsole &console) {
    console.handle([this](FakeConsole &c, const std::string &line) { serve(c, line); });
  }

  void serve(FakeConsole &c, const std::string &line) {
    const std::string name = quotedArg(line, "name");
    if (line.rfind("sendfile ", 0) == 0) {
      const size_t at = line.find("length=");
      const uint64_t length = parseNumber64(line.substr(at + 7)).value_or(0);
      if (sendfileStatus != 204) {
        c.line(std::to_string(sendfileStatus) + "- refused");
        return;
      }
      c.line("204- send binary data");
      c.expectBinary(length, [this, name](FakeConsole &console, const ut::Bytes &data) {
        if (afterDataStatus == 200) files[name] = data;
        console.line(std::to_string(afterDataStatus) + "- after data");
      });
    } else if (line.rfind("getfileattributes ", 0) == 0) {
      if (std::find(folders.begin(), folders.end(), name) != folders.end()) {
        c.line("200- sizehi=0x0 sizelo=0x0 directory");
      } else if (files.count(name)) {
        c.line("200- sizehi=0x0 sizelo=" + formatNumber(files[name].size()));
      } else {
        c.line("402- file not found");
      }
    } else if (line.rfind("delete ", 0) == 0) {
      if (files.erase(name)) c.line("200- OK");
      else c.line("402- file not found");
    } else if (line.rfind("rename ", 0) == 0) {
      if (renameStatus != 200) {
        c.line(std::to_string(renameStatus) + "- no");
        return;
      }
      const std::string to = quotedArg(line, "newname");
      auto it = files.find(name);
      if (it == files.end()) {
        c.line("402- file not found");
        return;
      }
      files[to] = it->second;
      files.erase(name);
      c.line("200- OK");
    } else if (line.rfind("getfile ", 0) == 0) {
      auto it = files.find(name);
      if (it == files.end()) {
        c.line("402- file not found");
        return;
      }
      c.line("203- binary response follows");
      c.send(le32(static_cast<uint32_t>(it->second.size())));
      c.send(it->second);
    } else {
      c.line("407- unknown command");
    }
  }

  std::vector<std::string> names() const {
    std::vector<std::string> out;
    for (const auto &[name, data] : files) out.push_back(name);
    return out;
  }
};

bool isTemporaryFor(const std::string &temp, const std::string &folder, const std::string &name) {
  if (temp.rfind(folder + "\\", 0) != 0) return false;
  const std::string leaf = temp.substr(folder.size() + 1);
  return leaf.size() <= kMaxFileNameBytes && leaf.size() > 14 && leaf.substr(leaf.size() - 5) == ".part" &&
         name.rfind(leaf.substr(0, leaf.size() - 14), 0) == 0;
}

} // namespace

TEST(XbdmTransfer, DownloadStreamsAndGivesTheConnectionBack) {
  const ut::Bytes file = ut::patternBytes(100000, 11);
  Files fs;
  fs.files["HDD:\\data.bin"] = file;
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  auto reader = client.openRead("HDD:\\data.bin", file.size());
  REQUIRE_OK(reader);
  CHECK_EQ(reader->size(), uint64_t{100000});
  CHECK(reader->isOpen());
  ut::Bytes got;
  std::vector<uint8_t> buffer(4093);
  for (;;) {
    auto n = reader->read(buffer);
    REQUIRE_OK(n);
    if (*n == 0) break;
    got.insert(got.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*n));
    CHECK_EQ(reader->position(), uint64_t{got.size()});
  }
  CHECK_EQ(got, file);
  CHECK(!reader->isOpen());
  CHECK_EQ(reader->read(buffer).value_or(1), size_t{0});
  CHECK(!client.transferActive());
  CHECK(client.isConnected());
  CHECK_EQ(console->commands(), std::vector<std::string>{"getfile name=\"HDD:\\data.bin\""});
}

TEST(XbdmTransfer, EmptyAndRefusedDownloads) {
  Files fs;
  fs.files["HDD:\\empty"] = {};
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  auto empty = client.openRead("HDD:\\empty");
  REQUIRE_OK(empty);
  CHECK_EQ(empty->size(), uint64_t{0});
  CHECK(!empty->isOpen());
  std::vector<uint8_t> buffer(8);
  CHECK_EQ(empty->read(buffer).value_or(1), size_t{0});
  CHECK(!client.transferActive());

  auto missing = client.openRead("HDD:\\missing");
  REQUIRE_ERR(missing, ErrorCode::Io);
  CHECK_EQ(consoleStatusCode(missing.error()).value_or(0), 402);
  CHECK(client.isConnected());

  CHECK_ERR(client.openRead("HDD:\\big", uint64_t{1} << 32), ErrorCode::Unsupported);
  CHECK_ERR(client.openRead("HDD:\\"), ErrorCode::InvalidArgument);
  CHECK_EQ(console->commands().size(), size_t{2});
}

TEST(XbdmTransfer, ClosingADownloadEarlyDropsTheConnectionAndReconnectRestoresIt) {
  Files fs;
  fs.files["HDD:\\f"] = ut::patternBytes(1000, 2);
  auto first = FakeConsole::create();
  fs.install(*first);
  auto second = FakeConsole::create();
  second->on("dbgname", "200- back\r\n");
  auto queue = std::make_shared<xt::ConsoleQueue>(xt::ConsoleQueue{second});
  auto client = connected(first, xt::quickOptions(), xt::connectorFor(queue));

  auto reader = client.openRead("HDD:\\f");
  REQUIRE_OK(reader);
  std::vector<uint8_t> buffer(10);
  REQUIRE_OK(reader->read(buffer));
  reader->close();
  CHECK(!reader->isOpen());
  CHECK_ERR(reader->read(buffer), ErrorCode::NotConnected);
  CHECK(!client.isConnected());
  CHECK(!client.transferActive());
  CHECK(first->closed());
  CHECK_EQ(first->byes(), 0);
  CHECK_ERR(client.debugName(), ErrorCode::NotConnected);

  REQUIRE_OK(client.reconnect());
  CHECK_EQ(client.debugName().value_or(""), std::string("back"));
  CHECK_ERR(client.reconnect(), ErrorCode::ConnectFailed);
}

TEST(XbdmTransfer, DownloadToFileIsAtomic) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const ut::Bytes file = ut::patternBytes(70000, 12);
  Files fs;
  fs.files["HDD:\\f.bin"] = file;
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  std::vector<std::pair<uint64_t, uint64_t>> progress;
  REQUIRE_OK(client.downloadToFile("HDD:\\f.bin", dir.file("out.bin"),
                                   [&](uint64_t done, uint64_t total) { progress.emplace_back(done, total); }));
  CHECK_EQ(ut::readFile(dir.file("out.bin")).value_or(ut::Bytes{}), file);
  CHECK_EQ(dir.entries(), std::vector<std::string>{"out.bin"});
  REQUIRE(!progress.empty());
  CHECK_EQ(progress.back().first, uint64_t{70000});
  CHECK_EQ(progress.back().second, uint64_t{70000});

  auto dropping = FakeConsole::create();
  dropping->handle([](FakeConsole &c, const std::string &line) {
    if (line.rfind("getfileattributes ", 0) == 0) {
      c.line("200- sizehi=0x0 sizelo=0x1388");
      return;
    }
    c.line("203- binary response follows");
    c.send(le32(5000));
    c.send(ut::Bytes(1000, 1));
    c.hangUp();
  });
  auto other = connected(dropping);
  CHECK_ERR(other.downloadToFile("HDD:\\f.bin", dir.file("partial.bin")), ErrorCode::Disconnected);
  CHECK_EQ(dir.entries(), std::vector<std::string>{"out.bin"});
}

TEST(XbdmTransfer, DownloadToFileIsBoundedByTheFileSize) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  // A console that announces a 5 GiB file's size modulo 4 GiB would hand over a
  // cut file that looks complete.
  auto huge = FakeConsole::create();
  huge->on("getfileattributes name=\"HDD:\\huge.bin\"", "200- sizehi=0x1 sizelo=0x40000000\r\n");
  auto client = connected(huge);
  CHECK_ERR(client.downloadToFile("HDD:\\huge.bin", dir.file("huge.bin")), ErrorCode::Unsupported);
  CHECK(dir.entries().empty());
  CHECK_EQ(huge->problems(), std::string());

  auto longer = FakeConsole::create();
  longer->on("getfileattributes name=\"HDD:\\f.bin\"", "200- sizehi=0x0 sizelo=0x10\r\n");
  longer->on("getfile name=\"HDD:\\f.bin\"", "203- binary response follows\r\n" + ut::textOf(le32(0x20)));
  auto other = connected(longer);
  CHECK_ERR(other.downloadToFile("HDD:\\f.bin", dir.file("f.bin")), ErrorCode::LimitExceeded);
  CHECK(dir.entries().empty());

  // A console without getfileattributes: the size comes from the listing (section 3.4).
  auto old = FakeConsole::create();
  old->on("getfileattributes name=\"HDD:\\f.bin\"", "407- unknown command\r\n");
  old->on("dirlist name=\"HDD:\\\"",
          "202- multiline response follows\r\nname=\"F.BIN\" sizehi=0x0 sizelo=0x2\r\n.\r\n");
  old->on("getfile name=\"HDD:\\f.bin\"", "203- binary response follows\r\n" + ut::textOf(le32(3)) + "abc");
  auto third = connected(old);
  CHECK_ERR(third.downloadToFile("HDD:\\f.bin", dir.file("f.bin")), ErrorCode::LimitExceeded);
  CHECK(dir.entries().empty());
  CHECK_EQ(old->problems(), std::string());

  // Neither answer has a size: the download goes ahead without the bound.
  auto bare = FakeConsole::create();
  bare->on("getfileattributes name=\"HDD:\\f.bin\"", "200- OK\r\n");
  bare->on("dirlist name=\"HDD:\\\"", "414- access denied\r\n");
  bare->on("getfile name=\"HDD:\\f.bin\"", "203- binary response follows\r\n" + ut::textOf(le32(3)) + "abc");
  auto fourth = connected(bare);
  REQUIRE_OK(fourth.downloadToFile("HDD:\\f.bin", dir.file("f.bin")));
  CHECK_EQ(ut::readFile(dir.file("f.bin")).value_or(ut::Bytes{}), ut::bytesOf("abc"));
  CHECK_EQ(bare->problems(), std::string());

  // A 202 without sizehi and sizelo, completed from the listing.
  auto partial = FakeConsole::create();
  partial->on("getfileattributes name=\"HDD:\\d\\g.bin\"",
              "202- multiline response follows\r\ncreatehi=0x1 createlo=0x2\r\n.\r\n");
  partial->on("dirlist name=\"HDD:\\d\\\"",
              "202- multiline response follows\r\nname=\"other\" sizehi=0x0 sizelo=0x9\r\n"
              "name=\"g.bin\" sizehi=0x0 sizelo=0x5\r\n.\r\n");
  partial->on("getfile name=\"HDD:\\d\\g.bin\"", "203- binary response follows\r\n" + ut::textOf(le32(5)) + "hello");
  auto fifth = connected(partial);
  REQUIRE_OK(fifth.downloadToFile("HDD:\\d\\g.bin", dir.file("g.bin")));
  CHECK_EQ(ut::readFile(dir.file("g.bin")).value_or(ut::Bytes{}), ut::bytesOf("hello"));
  CHECK_EQ(partial->problems(), std::string());
}

TEST(XbdmTransfer, DownloadToFileLeavesAFileNamedLikeItsTemporaryFileAlone) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  REQUIRE(ut::writeFile(dir.file("out.bin.part"), ut::bytesOf("mine")));
  Files fs;
  fs.files["HDD:\\f.bin"] = ut::bytesOf("console");
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  REQUIRE_OK(client.downloadToFile("HDD:\\f.bin", dir.file("out.bin")));
  CHECK_EQ(ut::readFile(dir.file("out.bin")).value_or(ut::Bytes{}), ut::bytesOf("console"));
  CHECK_EQ(ut::readFile(dir.file("out.bin.part")).value_or(ut::Bytes{}), ut::bytesOf("mine"));
  CHECK_ERR(client.downloadToFile("HDD:\\missing.bin", dir.file("out.bin")), ErrorCode::Io);
  CHECK_EQ(ut::readFile(dir.file("out.bin.part")).value_or(ut::Bytes{}), ut::bytesOf("mine"));
  CHECK_EQ(dir.entries(), (std::vector<std::string>{"out.bin", "out.bin.part"}));
}

TEST(XbdmTransfer, UploadGoesToATemporaryNameAndIsRenamedAfterTheConsoleConfirmed) {
  const ut::Bytes data = ut::patternBytes(150000, 13);
  Files fs;
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  auto writer = client.openWrite("HDD:\\Games\\a rather long file name for FATX.bin", data.size());
  REQUIRE_OK(writer);
  const std::string temp = writer->temporaryPath();
  CHECK(isTemporaryFor(temp, "HDD:\\Games", "a rather long file name for FATX.bin"));
  CHECK_EQ(writer->path(), std::string("HDD:\\Games\\a rather long file name for FATX.bin"));
  CHECK_EQ(console->lastCommand(), "sendfile name=\"" + temp + "\" length=0x249f0");
  CHECK(client.transferActive());
  CHECK_ERR(client.debugName(), ErrorCode::InvalidArgument);

  CHECK_ERR(writer->finish(), ErrorCode::InvalidArgument);
  CHECK(writer->isOpen());
  for (size_t offset = 0; offset < data.size(); offset += 30000) {
    REQUIRE_OK(writer->write(std::span<const uint8_t>(data).subspan(offset, 30000)));
  }
  CHECK_ERR(writer->write(ut::Bytes{1}), ErrorCode::InvalidArgument);
  CHECK_EQ(writer->written(), uint64_t{data.size()});
  REQUIRE_OK(writer->finish());
  CHECK(!writer->isOpen());
  CHECK(!client.transferActive());
  CHECK_EQ(fs.names(), std::vector<std::string>{"HDD:\\Games\\a rather long file name for FATX.bin"});
  CHECK_EQ(fs.files.begin()->second, data);
  const std::vector<std::string> expected = {
      "getfileattributes name=\"HDD:\\Games\\a rather long file name for FATX.bin\"",
      "sendfile name=\"" + temp + "\" length=0x249f0",
      "getfileattributes name=\"HDD:\\Games\\a rather long file name for FATX.bin\"",
      "rename name=\"" + temp + "\" newname=\"HDD:\\Games\\a rather long file name for FATX.bin\"",
  };
  CHECK_EQ(console->commands(), expected);
  CHECK(client.pendingCleanup().empty());
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmTransfer, UploadReplacesAnExistingFile) {
  Files fs;
  fs.files["HDD:\\x.txt"] = ut::bytesOf("old");
  auto console = FakeConsole::create();
  fs.install(*console);
  console->setMaxRead(1);
  auto client = connected(console);
  auto writer = client.openWrite("HDD:\\x.txt", 3);
  REQUIRE_OK(writer);
  REQUIRE_OK(writer->write(ut::bytesOf("new")));
  REQUIRE_OK(writer->finish());
  CHECK_EQ(fs.names(), std::vector<std::string>{"HDD:\\x.txt"});
  CHECK_EQ(fs.files["HDD:\\x.txt"], ut::bytesOf("new"));
  REQUIRE_EQ(console->commands().size(), size_t{5});
  CHECK_EQ(console->commands()[3], std::string("delete name=\"HDD:\\x.txt\""));
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmTransfer, AFailedRenameAfterTheOldFileWasDeletedKeepsTheUpload) {
  // Refused: the temporary file is the only copy of either version.
  {
    Files fs;
    fs.files["HDD:\\x.txt"] = ut::bytesOf("old");
    fs.renameStatus = 414;
    auto console = FakeConsole::create();
    fs.install(*console);
    auto client = connected(console);
    auto writer = client.openWrite("HDD:\\x.txt", 3);
    REQUIRE_OK(writer);
    const std::string temp = writer->temporaryPath();
    REQUIRE_OK(writer->write(ut::bytesOf("new")));
    auto r = writer->finish();
    REQUIRE(!r);
    CHECK_EQ(consoleStatusCode(r.error()).value_or(0), 414);
    CHECK_MSG(r.error().message.find("kept as " + temp) != std::string::npos, r.error().message);
    CHECK_EQ(fs.names(), std::vector<std::string>{temp});
    CHECK_EQ(fs.files[temp], ut::bytesOf("new"));
    CHECK(console->lastCommand().rfind("rename ", 0) == 0);
    CHECK(client.isConnected());
    CHECK(client.pendingCleanup().empty());
    CHECK_EQ(writer->keptPath().value_or(""), temp);
    CHECK_EQ(client.keptUploads(), std::vector<std::string>{temp});
    client.clearKeptUploads();
    CHECK(client.keptUploads().empty());
    CHECK_EQ(writer->keptPath().value_or(""), temp);
  }
  // The connection drops at the rename, or at the delete before it: whether
  // either happened is unknown, so nothing is queued for deletion.
  for (const std::string dropAt : {"rename ", "delete "}) {
    Files fs;
    fs.files["HDD:\\x.txt"] = ut::bytesOf("old");
    auto console = FakeConsole::create();
    console->handle([&fs, dropAt](FakeConsole &c, const std::string &line) {
      if (line.rfind(dropAt, 0) == 0) {
        c.hangUp();
        return;
      }
      fs.serve(c, line);
    });
    auto next = FakeConsole::create();
    fs.install(*next);
    auto queue = std::make_shared<xt::ConsoleQueue>(xt::ConsoleQueue{next});
    auto client = connected(console, xt::quickOptions(), xt::connectorFor(queue));
    auto writer = client.openWrite("HDD:\\x.txt", 3);
    REQUIRE_OK(writer);
    const std::string temp = writer->temporaryPath();
    REQUIRE_OK(writer->write(ut::bytesOf("new")));
    auto r = writer->finish();
    REQUIRE_ERR(r, ErrorCode::Disconnected);
    CHECK_MSG(r.error().message.find("kept as " + temp) != std::string::npos, r.error().message);
    CHECK(client.pendingCleanup().empty());
    CHECK_EQ(writer->keptPath().value_or(""), temp);
    CHECK_EQ(client.keptUploads(), std::vector<std::string>{temp});
    REQUIRE_OK(client.reconnect());
    CHECK(next->commands().empty());
    CHECK(fs.files.count(temp) == 1);
    CHECK_EQ(client.keptUploads(), std::vector<std::string>{temp});
  }
  // Refused before the old file was deleted: it is still there, so the
  // temporary file goes.
  {
    Files fs;
    fs.files["HDD:\\x.txt"] = ut::bytesOf("old");
    auto console = FakeConsole::create();
    console->handle([&fs](FakeConsole &c, const std::string &line) {
      if (line == "delete name=\"HDD:\\x.txt\"") {
        c.line("414- access denied");
        return;
      }
      fs.serve(c, line);
    });
    auto client = connected(console);
    auto writer = client.openWrite("HDD:\\x.txt", 3);
    REQUIRE_OK(writer);
    REQUIRE_OK(writer->write(ut::bytesOf("new")));
    CHECK_ERR(writer->finish(), ErrorCode::Io);
    CHECK_EQ(fs.names(), std::vector<std::string>{"HDD:\\x.txt"});
    CHECK_EQ(fs.files["HDD:\\x.txt"], ut::bytesOf("old"));
    CHECK(!writer->keptPath().has_value());
    CHECK(client.keptUploads().empty());
  }
}

TEST(XbdmTransfer, UploadFromFileReportsAKeptUpload) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  REQUIRE(ut::writeFile(dir.file("in.bin"), ut::bytesOf("new")));
  Files fs;
  fs.files["HDD:\\x.txt"] = ut::bytesOf("old");
  fs.renameStatus = 414;
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  auto r = client.uploadFromFile(dir.file("in.bin"), "HDD:\\x.txt");
  REQUIRE(!r);
  const auto kept = client.keptUploads();
  REQUIRE_EQ(kept.size(), size_t{1});
  CHECK(isTemporaryFor(kept[0], "HDD:", "x.txt"));
  CHECK_EQ(fs.names(), kept);
}

TEST(XbdmTransfer, ADeleteThatWasNeverSentDoesNotKeepTheUpload) {
  Files fs;
  fs.files["HDD:\\x.txt"] = ut::bytesOf("old");
  auto console = FakeConsole::create();
  int lookups = 0;
  console->handle([&fs, &lookups](FakeConsole &c, const std::string &line) {
    fs.serve(c, line);
    // After finish()'s lookup the connection drops before a byte of the next line,
    // the delete, goes out.
    if (line.rfind("getfileattributes ", 0) == 0 && ++lookups == 2) c.dropAfterWritten(c.written());
  });
  auto client = connected(console);
  auto writer = client.openWrite("HDD:\\x.txt", 3);
  REQUIRE_OK(writer);
  const std::string temp = writer->temporaryPath();
  REQUIRE_OK(writer->write(ut::bytesOf("new")));
  auto r = writer->finish();
  REQUIRE(!r);
  CHECK_MSG(r.error().message.find("kept as") == std::string::npos, r.error().message);
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK_EQ(delivery->command, std::string("delete"));
  CHECK(delivery->delivery == Delivery::NotSent);
  CHECK_EQ(client.pendingCleanup(), std::vector<std::string>{temp});
  CHECK_EQ(fs.files["HDD:\\x.txt"], ut::bytesOf("old"));
  CHECK(!writer->keptPath().has_value());
  CHECK(client.keptUploads().empty());
}

TEST(XbdmTransfer, UploadFindsTheFileToReplaceInTheListingWithoutGetfileattributes) {
  Files fs;
  fs.files["HDD:\\d\\x.txt"] = ut::bytesOf("old");
  auto console = FakeConsole::create();
  console->handle([&fs](FakeConsole &c, const std::string &line) {
    if (line.rfind("getfileattributes ", 0) == 0) {
      c.line("407- unknown command");
    } else if (line == "dirlist name=\"HDD:\\d\\\"") {
      c.multiline({"name=\"X.TXT\" sizehi=0x0 sizelo=0x3"});
    } else {
      fs.serve(c, line);
    }
  });
  auto client = connected(console);
  auto writer = client.openWrite("HDD:\\d\\x.txt", 3);
  REQUIRE_OK(writer);
  REQUIRE_OK(writer->write(ut::bytesOf("new")));
  REQUIRE_OK(writer->finish());
  CHECK_EQ(fs.names(), std::vector<std::string>{"HDD:\\d\\x.txt"});
  CHECK_EQ(fs.files["HDD:\\d\\x.txt"], ut::bytesOf("new"));
  // The listing is read in openWrite() and again in finish().
  REQUIRE_EQ(console->commands().size(), size_t{7});
  CHECK_EQ(console->commands()[1], std::string("dirlist name=\"HDD:\\d\\\""));
  CHECK_EQ(console->commands()[5], std::string("delete name=\"HDD:\\d\\x.txt\""));
}

TEST(XbdmTransfer, UploadOntoAFolderFailsBeforeTheData) {
  Files fs;
  fs.folders.push_back("HDD:\\dir");
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  CHECK_ERR(client.openWrite("HDD:\\dir", 2), ErrorCode::InvalidArgument);
  CHECK_EQ(console->commands(), std::vector<std::string>{"getfileattributes name=\"HDD:\\dir\""});
  CHECK(fs.files.empty());
  CHECK(client.isConnected());
  CHECK(!client.transferActive());
  CHECK(client.pendingCleanup().empty());
}

TEST(XbdmTransfer, AFolderThatAppearsDuringTheUploadFailsAndRemovesTheTemporaryFile) {
  Files fs;
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  auto writer = client.openWrite("HDD:\\dir", 2);
  REQUIRE_OK(writer);
  fs.folders.push_back("HDD:\\dir");
  REQUIRE_OK(writer->write(ut::Bytes{1, 2}));
  CHECK_ERR(writer->finish(), ErrorCode::InvalidArgument);
  CHECK(fs.files.empty());
  CHECK_EQ(console->lastCommand(), "delete name=\"" + writer->temporaryPath() + "\"");
  CHECK(client.isConnected());
  CHECK(client.pendingCleanup().empty());
}

TEST(XbdmTransfer, AFileThatAppearsDuringTheUploadIsNotReplaced) {
  Files fs;
  auto console = FakeConsole::create();
  console->handle([&fs](FakeConsole &c, const std::string &line) {
    fs.serve(c, line);
    // Another client creates the final name while the data is on its way.
    if (line.rfind("sendfile ", 0) == 0) fs.files["HDD:\\x.txt"] = ut::bytesOf("theirs");
  });
  auto client = connected(console);
  auto writer = client.openWrite("HDD:\\x.txt", 3);
  REQUIRE_OK(writer);
  const std::string temp = writer->temporaryPath();
  REQUIRE_OK(writer->write(ut::bytesOf("new")));
  auto r = writer->finish();
  REQUIRE_ERR(r, ErrorCode::InvalidArgument);
  CHECK_MSG(r.error().message.find("HDD:\\x.txt appeared during the upload and was not replaced; the upload was removed") !=
                std::string::npos,
            r.error().message);
  CHECK_EQ(fs.names(), std::vector<std::string>{"HDD:\\x.txt"});
  CHECK_EQ(fs.files["HDD:\\x.txt"], ut::bytesOf("theirs"));
  CHECK_EQ(console->lastCommand(), "delete name=\"" + temp + "\"");
  for (const auto &command : console->commands()) CHECK(command != "delete name=\"HDD:\\x.txt\"");
  CHECK(client.isConnected());
  CHECK(client.pendingCleanup().empty());
  CHECK(!writer->keptPath().has_value());
  CHECK(client.keptUploads().empty());
  CHECK_OK(client.attributes("HDD:\\x.txt"));
}

TEST(XbdmTransfer, AFileThatAppearsBeforeTheRenameIsNotReplaced) {
  Files fs;
  auto console = FakeConsole::create();
  console->handle([&fs](FakeConsole &c, const std::string &line) {
    if (line.rfind("rename ", 0) == 0) {
      // The final name appears between finish()'s lookup and the rename.
      fs.files["HDD:\\x.txt"] = ut::bytesOf("theirs");
      c.line("410- already exists");
      return;
    }
    fs.serve(c, line);
  });
  auto client = connected(console);
  auto writer = client.openWrite("HDD:\\x.txt", 3);
  REQUIRE_OK(writer);
  const std::string temp = writer->temporaryPath();
  REQUIRE_OK(writer->write(ut::bytesOf("new")));
  auto r = writer->finish();
  REQUIRE_ERR(r, ErrorCode::InvalidArgument);
  CHECK_MSG(r.error().message.find("appeared during the upload") != std::string::npos, r.error().message);
  CHECK_EQ(fs.files["HDD:\\x.txt"], ut::bytesOf("theirs"));
  CHECK_EQ(fs.files.count(temp), size_t{0});
  CHECK_EQ(console->lastCommand(), "delete name=\"" + temp + "\"");
  CHECK(client.pendingCleanup().empty());
}

TEST(XbdmTransfer, AFileReplacedDuringTheUploadIsStillReplaced) {
  Files fs;
  fs.files["HDD:\\x.txt"] = ut::bytesOf("old");
  auto console = FakeConsole::create();
  console->handle([&fs](FakeConsole &c, const std::string &line) {
    fs.serve(c, line);
    if (line.rfind("sendfile ", 0) == 0) fs.files["HDD:\\x.txt"] = ut::bytesOf("other");
  });
  auto client = connected(console);
  auto writer = client.openWrite("HDD:\\x.txt", 3);
  REQUIRE_OK(writer);
  REQUIRE_OK(writer->write(ut::bytesOf("new")));
  REQUIRE_OK(writer->finish());
  CHECK_EQ(fs.names(), std::vector<std::string>{"HDD:\\x.txt"});
  CHECK_EQ(fs.files["HDD:\\x.txt"], ut::bytesOf("new"));
}

TEST(XbdmTransfer, RefusalsBeforeAndAfterTheData) {
  {
    Files fs;
    fs.sendfileStatus = 413;
    auto console = FakeConsole::create();
    fs.install(*console);
    auto client = connected(console);
    auto writer = client.openWrite("HDD:\\nofolder\\x", 4);
    REQUIRE_ERR(writer, ErrorCode::Io);
    CHECK_EQ(consoleStatusCode(writer.error()).value_or(0), 413);
    CHECK(client.isConnected());
    CHECK(!client.transferActive());
  }
  {
    Files fs;
    fs.afterDataStatus = 415;
    auto console = FakeConsole::create();
    fs.install(*console);
    auto client = connected(console);
    auto writer = client.openWrite("HDD:\\full.bin", 4);
    REQUIRE_OK(writer);
    REQUIRE_OK(writer->write(ut::Bytes{1, 2, 3, 4}));
    auto done = writer->finish();
    REQUIRE_ERR(done, ErrorCode::Io);
    CHECK_EQ(consoleStatusCode(done.error()).value_or(0), 415);
    CHECK_EQ(console->lastCommand(), "delete name=\"" + writer->temporaryPath() + "\"");
    CHECK(client.isConnected());
    CHECK(fs.files.empty());
    // Only looked up, never changed.
    for (const auto &command : console->commands()) {
      if (command.rfind("getfileattributes ", 0) != 0) CHECK(command.find("HDD:\\full.bin\"") == std::string::npos);
    }
  }
  {
    Files fs;
    fs.renameStatus = 414;
    auto console = FakeConsole::create();
    fs.install(*console);
    auto client = connected(console);
    auto writer = client.openWrite("HDD:\\r.bin", 1);
    REQUIRE_OK(writer);
    REQUIRE_OK(writer->write(ut::Bytes{9}));
    auto done = writer->finish();
    REQUIRE(!done);
    CHECK_EQ(consoleStatusCode(done.error()).value_or(0), 414);
    CHECK(fs.files.empty());
    CHECK_EQ(console->lastCommand(), "delete name=\"" + writer->temporaryPath() + "\"");
  }
}

TEST(XbdmTransfer, DropMidUploadLeavesNothingUnderTheFinalNameAndReconnectCleansUp) {
  Files fs;
  auto first = FakeConsole::create();
  fs.install(*first);
  first->dropAfterWritten(60 + 5000);
  auto second = FakeConsole::create();
  std::vector<std::string> deleted;
  second->handle([&](FakeConsole &c, const std::string &line) {
    deleted.push_back(line);
    c.line("200- OK");
  });
  auto queue = std::make_shared<xt::ConsoleQueue>(xt::ConsoleQueue{second});
  auto client = connected(first, xt::quickOptions(), xt::connectorFor(queue));

  const ut::Bytes data = ut::patternBytes(20000, 14);
  auto writer = client.openWrite("HDD:\\keep.bin", data.size());
  REQUIRE_OK(writer);
  const std::string temp = writer->temporaryPath();
  Result<void> sent;
  for (size_t offset = 0; offset < data.size() && sent; offset += 4096) {
    sent = writer->write(std::span<const uint8_t>(data).subspan(offset, std::min<size_t>(4096, data.size() - offset)));
  }
  REQUIRE_ERR(sent, ErrorCode::Disconnected);
  CHECK(!writer->isOpen());
  CHECK(!client.isConnected());
  CHECK(!client.transferActive());
  CHECK_EQ(client.pendingCleanup(), std::vector<std::string>{temp});
  CHECK(fs.files.empty());
  CHECK(!writer->keptPath().has_value());
  CHECK(client.keptUploads().empty());
  for (const auto &command : first->commands()) {
    if (command.rfind("getfileattributes ", 0) != 0) CHECK(command.find("keep.bin\"") == std::string::npos);
  }
  CHECK_ERR(writer->finish(), ErrorCode::NotConnected);

  REQUIRE_OK(client.reconnect());
  CHECK_EQ(deleted, std::vector<std::string>{"delete name=\"" + temp + "\""});
  CHECK(client.pendingCleanup().empty());
  CHECK(client.isConnected());
}

TEST(XbdmTransfer, ALostSendfileAnswerQueuesTheTemporaryName) {
  auto first = FakeConsole::create();
  first->handle([](FakeConsole &c, const std::string &line) {
    if (line.rfind("getfileattributes ", 0) == 0) {
      c.line("402- file not found");
      return;
    }
    c.send(std::string_view("204- send bin"));
    c.hangUp();
  });
  auto second = FakeConsole::create();
  std::vector<std::string> deleted;
  second->handle([&](FakeConsole &c, const std::string &line) {
    deleted.push_back(line);
    c.line("402- file not found");
  });
  auto queue = std::make_shared<xt::ConsoleQueue>(xt::ConsoleQueue{second});
  auto client = connected(first, xt::quickOptions(), xt::connectorFor(queue));
  CHECK_ERR(client.openWrite("HDD:\\up.bin", 10), ErrorCode::Disconnected);
  REQUIRE_EQ(client.pendingCleanup().size(), size_t{1});
  const std::string temp = client.pendingCleanup().front();
  CHECK(isTemporaryFor(temp, "HDD:", "up.bin"));
  REQUIRE_EQ(first->commands().size(), size_t{2});
  CHECK_EQ(first->commands()[1].find("sendfile name=\"" + temp + "\""), size_t{0});
  REQUIRE_OK(client.reconnect());
  CHECK_EQ(deleted, std::vector<std::string>{"delete name=\"" + temp + "\""});
  CHECK(client.pendingCleanup().empty());

  auto refusing = FakeConsole::create();
  refusing->handle([](FakeConsole &c, const std::string &) { c.line("413- file cannot be created"); });
  auto other = connected(refusing);
  CHECK_ERR(other.openWrite("HDD:\\up.bin", 10), ErrorCode::Io);
  CHECK(other.pendingCleanup().empty());
}

TEST(XbdmTransfer, AbortAndDestructionCloseTheConnection) {
  Files fs;
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  {
    auto writer = client.openWrite("HDD:\\a.bin", 10);
    REQUIRE_OK(writer);
    REQUIRE_OK(writer->write(ut::Bytes{1, 2, 3}));
  }
  CHECK(!client.isConnected());
  CHECK_EQ(client.pendingCleanup().size(), size_t{1});
  CHECK(fs.files.empty());

  Files other;
  auto second = FakeConsole::create();
  other.install(*second);
  auto client2 = connected(second);
  auto writer = client2.openWrite("HDD:\\b.bin", 10);
  REQUIRE_OK(writer);
  writer->abort();
  writer->abort();
  CHECK(!writer->isOpen());
  CHECK(!client2.isConnected());
  CHECK_ERR(writer->write(ut::Bytes{1}), ErrorCode::NotConnected);
  CHECK_EQ(client2.pendingCleanup(), std::vector<std::string>{writer->temporaryPath()});
  CHECK_ERR(client2.reconnect(), ErrorCode::Unsupported);
}

TEST(XbdmTransfer, UploadSizesAndLimits) {
  Files fs;
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  const uint64_t fiveGiB = uint64_t{5} << 30;
  CHECK_ERR(client.openWrite("HDD:\\huge.bin", fiveGiB), ErrorCode::LimitExceeded);
  CHECK(console->commands().empty());

  ClientOptions wide = xt::quickOptions();
  wide.maxUploadBytes = UINT64_MAX;
  client.setOptions(wide);
  auto writer = client.openWrite("HDD:\\huge.bin", fiveGiB);
  REQUIRE_OK(writer);
  CHECK_EQ(writer->size(), fiveGiB);
  CHECK(console->lastCommand().find(" length=0x140000000") != std::string::npos);
  writer->abort();

  CHECK_ERR(client.openWrite("HDD:\\", 1), ErrorCode::InvalidArgument);
}

TEST(XbdmTransfer, ZeroLengthUploads) {
  Files fs;
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  auto writer = client.openWrite("HDD:\\zero", 0);
  REQUIRE_OK(writer);
  REQUIRE_OK(writer->finish());
  CHECK_EQ(fs.names(), std::vector<std::string>{"HDD:\\zero"});

  auto direct = FakeConsole::create();
  direct->handle([](FakeConsole &c, const std::string &line) {
    if (line.rfind("sendfile", 0) == 0) c.line("200- OK");
    else if (line.rfind("getfileattributes", 0) == 0) c.line("402- no");
    else c.line("200- OK");
  });
  auto other = connected(direct);
  auto confirmed = other.openWrite("HDD:\\zero", 0);
  REQUIRE_OK(confirmed);
  REQUIRE_OK(confirmed->finish());
  CHECK_EQ(direct->commands().size(), size_t{4});

  auto strict = FakeConsole::create();
  strict->handle([](FakeConsole &c, const std::string &) { c.line("200- OK"); });
  auto third = connected(strict);
  CHECK_ERR(third.openWrite("HDD:\\one", 1), ErrorCode::Protocol);
}

TEST(XbdmTransfer, UploadFromFile) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const ut::Bytes data = ut::patternBytes(90000, 15);
  REQUIRE(ut::writeFile(dir.file("in.bin"), data));
  Files fs;
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  uint64_t last = 0;
  REQUIRE_OK(client.uploadFromFile(dir.file("in.bin"), "HDD:\\in.bin", [&](uint64_t done, uint64_t) { last = done; }));
  CHECK_EQ(fs.files["HDD:\\in.bin"], data);
  CHECK_EQ(last, uint64_t{90000});
  CHECK_ERR(client.uploadFromFile(dir.file("missing"), "HDD:\\x", nullptr), ErrorCode::InvalidArgument);
  CHECK_ERR(client.uploadFromFile(dir.path(), "HDD:\\x", nullptr), ErrorCode::InvalidArgument);

  ut::TempDir out;
  REQUIRE_OK(client.downloadToFile("HDD:\\in.bin", out.file("back.bin")));
  CHECK_EQ(ut::readFile(out.file("back.bin")).value_or(ut::Bytes{}), data);
}

TEST(XbdmTransfer, ASecondClientServesAnotherTransfer) {
  Files fs;
  fs.files["HDD:\\one"] = ut::patternBytes(64, 1);
  fs.files["HDD:\\two"] = ut::patternBytes(64, 2);
  auto a = FakeConsole::create();
  auto b = FakeConsole::create();
  fs.install(*a);
  fs.install(*b);
  auto first = connected(a);
  auto second = connected(b);
  auto r1 = first.openRead("HDD:\\one");
  auto r2 = second.openRead("HDD:\\two");
  REQUIRE_OK(r1);
  REQUIRE_OK(r2);
  std::vector<uint8_t> buffer(64);
  CHECK_EQ(r1->read(buffer).value_or(0), size_t{64});
  CHECK_EQ(buffer, fs.files["HDD:\\one"]);
  CHECK_EQ(r2->read(buffer).value_or(0), size_t{64});
  CHECK_EQ(buffer, fs.files["HDD:\\two"]);
}

TEST(XbdmTransfer, ConnectOverLoopbackTcp) {
  std::string why;
  auto server = ut::LoopbackServer::start(
      [](ut::ServerConnection &connection, const std::atomic<bool> &) {
        connection.sendAll(ut::bytesOf("201- connected\r\n"));
        ut::Bytes command;
        if (!connection.recvExact(command, 9) || ut::textOf(command) != "dbgname\r\n") return;
        connection.sendAll(ut::bytesOf("200- Loopback Kit\r\n"));
        if (!connection.recvExact(command, 5) || ut::textOf(command) != "bye\r\n") return;
        connection.sendAll(ut::bytesOf("200- bye\r\n"));
      },
      &why);
  if (!server) SKIP("loopback sockets are not available here: " + why);
  auto endpoint = net::Endpoint::parse("xbdm://127.0.0.1:" + std::to_string(server->port()));
  REQUIRE_OK(endpoint);
  auto client = XbdmClient::connect(*endpoint, xt::quickOptions());
  REQUIRE_OK(client);
  CHECK_EQ(client->debugName().value_or(""), std::string("Loopback Kit"));
  client->close();
  CHECK(!client->isConnected());
}

TEST(XbdmTransfer, KeptUploadsAreBounded) {
  Files fs;
  fs.renameStatus = 414;
  auto console = FakeConsole::create();
  fs.install(*console);
  auto client = connected(console);
  std::string first;
  for (int i = 0; i < 65; ++i) {
    fs.files["HDD:\\x.txt"] = ut::bytesOf("old");
    auto writer = client.openWrite("HDD:\\x.txt", 1);
    REQUIRE_OK(writer);
    if (i == 0) first = writer->temporaryPath();
    REQUIRE_OK(writer->write(ut::Bytes{1}));
    REQUIRE(!writer->finish());
  }
  const auto kept = client.keptUploads();
  CHECK_EQ(kept.size(), size_t{64});
  CHECK(std::find(kept.begin(), kept.end(), first) == kept.end());
}
