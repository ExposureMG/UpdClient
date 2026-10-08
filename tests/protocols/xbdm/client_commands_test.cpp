#include "protocols/xbdm/client_fake.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <core/hex.hpp>
#include <protocols/xbdm/client.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace updclient;
using namespace updclient::xbdm;
using xt::FakeConsole;

namespace {

XbdmClient connected(const std::shared_ptr<FakeConsole> &console, ClientOptions options = xt::quickOptions()) {
  auto client = xt::attach(console, options);
  if (!client) throw std::runtime_error("attach failed: " + formatError(client.error()));
  return std::move(*client);
}

void expectStatus(const Error &error, int code) {
  CHECK_EQ(consoleStatusCode(error).value_or(0), code);
  CHECK_EQ(error.sysError, code);
  CHECK_MSG(error.message.find(std::to_string(code) + "- ") != std::string::npos, error.message);
}

} // namespace

TEST(XbdmClient, GreetingIsRequiredBeforeTheFirstCommand) {
  auto console = FakeConsole::create();
  console->on("dbgname", "200- Box\r\n");
  auto client = connected(console);
  CHECK(client.isConnected());
  CHECK_EQ(client.debugName().value_or(""), std::string("Box"));
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, GreetingTextOtherThanConnectedIsAccepted) {
  auto console = FakeConsole::create(false);
  console->line("201- CONNECTED to xbdm");
  CHECK_OK(xt::attach(console));
}

TEST(XbdmClient, GreetingFailures) {
  auto busy = FakeConsole::create(false);
  busy->line("401- max number of connections exceeded");
  auto r = xt::attach(busy);
  REQUIRE_ERR(r, ErrorCode::LimitExceeded);
  expectStatus(r.error(), 401);
  CHECK(busy->closed());

  auto odd = FakeConsole::create(false);
  odd->line("200- OK");
  CHECK_ERR(xt::attach(odd), ErrorCode::Protocol);
  CHECK(odd->closed());

  auto garbage = FakeConsole::create(false);
  garbage->line("hello");
  CHECK_ERR(xt::attach(garbage), ErrorCode::Protocol);

  auto silent = FakeConsole::create(false);
  CHECK_ERR(xt::attach(silent), ErrorCode::Timeout);
  CHECK(silent->closed());

  auto gone = FakeConsole::create(false);
  gone->hangUp();
  CHECK_ERR(xt::attach(gone), ErrorCode::Disconnected);

  CHECK_ERR(XbdmClient::attach(nullptr), ErrorCode::ConnectFailed);
}

TEST(XbdmClient, CloseSaysByeAndTheDestructorToo) {
  auto console = FakeConsole::create();
  {
    auto client = connected(console);
    client.close();
    CHECK(!client.isConnected());
    client.close();
  }
  CHECK_EQ(console->byes(), 1);
  CHECK_EQ(console->commands(), std::vector<std::string>{"bye"});

  auto other = FakeConsole::create();
  { auto client = connected(other); }
  CHECK_EQ(other->byes(), 1);
  CHECK(other->closed());
}

TEST(XbdmClient, ConsoleInformation) {
  auto console = FakeConsole::create();
  console->on("dbgname", "200- Dev Kit 1\r\n")
      .on("consoletype", "200- reviewerkit\r\n")
      .on("getconsoleid", "200- consoleid=0123456789AB\r\n")
      .on("xbeinfo running", "202- multiline response follows\r\ntimestamp=0x00000000 checksum=0x0000002a\r\n"
                             "name=\"\\Device\\Harddisk0\\SystemExtPartition\\20449700\\dash.xex\"\r\n.\r\n")
      .on("getexecstate", "200- pending_title\r\n")
      .on("altaddr", "200- addr=0xc0a80102\r\n")
      .on("xbeinfo name=\"HDD:\\Games\\a.xex\"", "202- multiline response follows\r\nname=\"HDD:\\Games\\a.xex\"\r\n.\r\n");
  auto client = connected(console);
  CHECK_EQ(client.debugName().value_or(""), std::string("Dev Kit 1"));
  CHECK_EQ(client.consoleType().value_or(""), std::string("reviewerkit"));
  CHECK_EQ(client.consoleId().value_or(""), std::string("0123456789AB"));
  auto title = client.runningTitle();
  REQUIRE_OK(title);
  CHECK_EQ(title->name, std::string("\\Device\\Harddisk0\\SystemExtPartition\\20449700\\dash.xex"));
  CHECK_EQ(title->checksum.value_or(0), uint32_t{42});
  CHECK_EQ(title->timestamp.value_or(1), uint32_t{0});
  auto state = client.execState();
  REQUIRE_OK(state);
  CHECK(state->state == ExecState::PendingTitle);
  CHECK_EQ(state->text, std::string("pending_title"));
  auto address = client.titleAddress();
  REQUIRE_OK(address);
  CHECK_EQ(address->text, std::string("192.168.1.2"));
  CHECK_EQ(address->raw, uint32_t{0xC0A80102});
  CHECK_EQ(client.executableInfo("HDD:\\Games\\a.xex").value_or(XbeInfo{}).name, std::string("HDD:\\Games\\a.xex"));
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, ExecStates) {
  const std::pair<const char *, ExecState> cases[] = {
      {"start", ExecState::Start},   {"stop", ExecState::Stop},
      {"pending", ExecState::Pending}, {"reboot", ExecState::Reboot},
      {"reboot_title", ExecState::RebootTitle}, {"START", ExecState::Start},
      {"weird", ExecState::Unknown},
  };
  for (const auto &[word, state] : cases) {
    auto console = FakeConsole::create();
    console->on("getexecstate", std::string("200- ") + word + "\r\n");
    auto client = connected(console);
    auto r = client.execState();
    REQUIRE_OK(r);
    CHECK(r->state == state);
  }
}

TEST(XbdmClient, ConsoleIdWithoutKeyIsTheText) {
  auto console = FakeConsole::create();
  console->on("getconsoleid", "200- ABCDEF\r\n");
  auto client = connected(console);
  CHECK_EQ(client.consoleId().value_or(""), std::string("ABCDEF"));
}

TEST(XbdmClient, ConsoleInfoHidesRefusedFields) {
  auto console = FakeConsole::create();
  console->on("dbgname", "200- Box\r\n")
      .on("consoletype", "407- unknown command\r\n")
      .on("getconsoleid", "407- unknown command\r\n")
      .on("xbeinfo running", "402- no such file\r\n")
      .on("getexecstate", "200- start\r\n")
      .on("altaddr", "400- error\r\n");
  auto client = connected(console);
  auto info = client.consoleInfo();
  REQUIRE_OK(info);
  CHECK_EQ(info->debugName.value_or(""), std::string("Box"));
  CHECK(!info->consoleType.has_value());
  CHECK(!info->consoleId.has_value());
  CHECK(!info->runningTitle.has_value());
  CHECK(info->execState.has_value());
  CHECK(!info->titleAddress.has_value());
  CHECK(client.isConnected());
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, ConsoleInfoStopsAtATransportFailure) {
  auto console = FakeConsole::create();
  console->on("dbgname", "200- Box\r\n").on("consoletype", "");
  auto client = connected(console);
  CHECK_ERR(client.consoleInfo(), ErrorCode::Timeout);
  CHECK(!client.isConnected());
}

TEST(XbdmClient, MalformedSingleLineAnswers) {
  auto console = FakeConsole::create();
  console->on("altaddr", "200- addr=banana\r\n").on("xbeinfo running", "202- x\r\ntimestamp=0x1\r\n.\r\n");
  auto client = connected(console);
  CHECK_ERR(client.titleAddress(), ErrorCode::Protocol);
  CHECK(client.isConnected());
  CHECK_ERR(client.runningTitle(), ErrorCode::Protocol);
  CHECK(client.isConnected());
}

TEST(XbdmClient, DrivesAndFreeSpace) {
  auto console = FakeConsole::create();
  console->on("drivelist", "202- multiline response follows\r\ndrivename=\"DEVKIT\"\r\ndrivename=\"HDD\"\r\n"
                           "drivename=\"Z\"\r\nnonsense\r\ndrivename=\"BAD:\"\r\n.\r\n")
      .on("drivefreespace name=\"HDD:\\\"",
          "202- multiline response follows\r\nfreetocallerhi=0x00000003 freetocallerlo=0x1a2b0000 "
          "totalbyteshi=0x00000004 totalbyteslo=0x00000000 totalfreebyteshi=0x00000003 "
          "totalfreebyteslo=0x1a2b0001\r\n.\r\n")
      .on("drivefreespace name=\"Z:\\\"", "200- freetocallerhi=0x0 freetocallerlo=0x10 totalbyteshi=0x0 totalbyteslo=0x100\r\n")
      .on("drivefreespace name=\"E:\\\"", "202- multiline response follows\r\ntotalbyteshi=0x0 totalbyteslo=0x100\r\n.\r\n")
      .on("drivefreespace name=\"Y:\\\"", "414- access denied\r\n");
  auto client = connected(console);
  CHECK_EQ(client.drives().value_or(std::vector<std::string>{}), (std::vector<std::string>{"DEVKIT", "HDD", "Z"}));

  auto hdd = client.driveSpace("HDD");
  REQUIRE_OK(hdd);
  CHECK_EQ(hdd->freeToCaller, uint64_t{0x31a2b0000});
  CHECK_EQ(hdd->totalBytes, uint64_t{0x400000000});
  CHECK_EQ(hdd->totalFreeBytes, uint64_t{0x31a2b0001});
  CHECK_EQ(hdd->usedBytes(), uint64_t{0x400000000 - 0x31a2b0000});

  auto z = client.driveSpace("Z:\\");
  REQUIRE_OK(z);
  CHECK_EQ(z->freeToCaller, uint64_t{0x10});
  CHECK_EQ(z->totalFreeBytes, uint64_t{0x10});
  CHECK_ERR(client.driveSpace("E:"), ErrorCode::Protocol);
  auto denied = client.driveSpace("Y");
  REQUIRE_ERR(denied, ErrorCode::Io);
  expectStatus(denied.error(), 414);
  CHECK_ERR(client.driveSpace("H:D"), ErrorCode::InvalidArgument);
  CHECK(client.isConnected());
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, DrivesAreDistinctAndAtMost64) {
  auto console = FakeConsole::create();
  std::string body = "202- multiline response follows\r\n";
  for (int i = 0; i < 1000; ++i) body += "drivename=\"HDD\"\r\ndrivename=\"hdd\"\r\n";
  for (int i = 0; i < 100; ++i) body += "drivename=\"D" + std::to_string(i) + "\"\r\n";
  console->on("drivelist", body + ".\r\n");
  auto client = connected(console);
  auto drives = client.drives();
  REQUIRE_OK(drives);
  REQUIRE_EQ(drives->size(), size_t{64});
  CHECK_EQ((*drives)[0], std::string("HDD"));
  CHECK_EQ((*drives)[1], std::string("D0"));
  CHECK_EQ((*drives)[63], std::string("D62"));
  CHECK(client.isConnected());
}

TEST(XbdmClient, DirectoryListing) {
  auto console = FakeConsole::create();
  console->on("dirlist name=\"HDD:\\Content\\\"",
              "202- multiline response follows\r\n"
              "name=\"0000000000000000\" sizehi=0x0 sizelo=0x0 createhi=0x01d11fb5 createlo=0x59683c00 "
              "changehi=0x01d11fb5 changelo=0x59683c01 directory\r\n"
              "name=\"default.xex\" sizehi=0x0 sizelo=0x0004f000 createhi=0x01d11fb5 createlo=0x59683c00 "
              "changehi=0x01d11fb5 changelo=0x59683c00\r\n"
              "name=\"big.bin\" changehi=0x1 changelo=0x2 sizelo=0x00000010 sizehi=0x00000002 readonly hidden "
              "futureflag future=1\r\n"
              "name=\"no size\"\r\n"
              "name=\".\" directory\r\n"
              "name=\"..\" directory\r\n"
              "sizehi=0x0 sizelo=0x1\r\n"
              "name=\"bad\" sizelo=0xZZ\r\n"
              "name=\"half\" createhi=0x1\r\n"
              "name=\"a\\b\"\r\n"
              "name=\"broken\r\n"
              ".\r\n");
  auto client = connected(console);
  auto listing = client.list("HDD:\\Content");
  REQUIRE_OK(listing);
  REQUIRE_EQ(listing->entries.size(), size_t{5});
  CHECK_EQ(listing->skipped, size_t{4});

  const auto &folder = listing->entries[0];
  CHECK_EQ(folder.name, std::string("0000000000000000"));
  CHECK(folder.isDirectory);
  CHECK_EQ(folder.createdFileTime.value_or(0), uint64_t{0x01d11fb559683c00});
  CHECK_EQ(folder.changedFileTime.value_or(0), uint64_t{0x01d11fb559683c01});
  REQUIRE(folder.created().has_value());
  CHECK_EQ(std::chrono::duration_cast<std::chrono::seconds>(folder.created()->time_since_epoch()).count(),
           int64_t{0x01d11fb559683c00 / 10000000 - 11644473600ll});

  const auto &xex = listing->entries[1];
  CHECK(!xex.isDirectory);
  CHECK_EQ(xex.size, uint64_t{0x4f000});

  const auto &big = listing->entries[2];
  CHECK_EQ(big.size, uint64_t{0x200000010});
  CHECK(big.isReadOnly);
  CHECK(big.isHidden);
  CHECK(!big.createdFileTime.has_value());
  CHECK_EQ(big.changedFileTime.value_or(0), uint64_t{0x100000002});

  CHECK_EQ(listing->entries[3].size, uint64_t{0});
  CHECK(!listing->entries[3].sizeKnown);
  CHECK(xex.sizeKnown);
  CHECK(!listing->entries[3].created().has_value());
  CHECK_EQ(listing->entries[4].name, std::string("half"));
  CHECK(!listing->entries[4].createdFileTime.has_value());
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, ListingSkipsNamesNoCommandCouldUse) {
  // Every name handed out can be joined to its folder and sent back.
  auto console = FakeConsole::create();
  std::string body = "202- multiline response follows\r\n";
  for (const char *name : {":", "C:", "C:x", "x:", "*", "a?", "<>|", "a\x7f", "caf\xc3\xa9", "a/b"}) {
    body += std::string("name=\"") + name + "\" sizehi=0x0 sizelo=0x1\r\n";
  }
  console->on("dirlist name=\"HDD:\\\"", body + "name=\"ok name.txt\" sizehi=0x0 sizelo=0x1\r\n.\r\n");
  auto client = connected(console);
  auto listing = client.list("HDD:\\");
  REQUIRE_OK(listing);
  REQUIRE_EQ(listing->entries.size(), size_t{1});
  CHECK_EQ(listing->entries[0].name, std::string("ok name.txt"));
  CHECK_EQ(listing->skipped, size_t{10});
  for (const auto &entry : listing->entries) CHECK_OK(joinPath("HDD:\\", entry.name));
}

TEST(XbdmClient, ListingSendsExactlyOneTrailingBackslash) {
  auto console = FakeConsole::create();
  console->on("dirlist name=\"HDD:\\\"", "202- multiline response follows\r\n.\r\n")
      .on("dirlist name=\"HDD:\\\"", "202- multiline response follows\r\n.\r\n")
      .on("dirlist name=\"HDD:\\a\\\"", "202- multiline response follows\r\n.\r\n")
      .on("dirlist name=\"HDD:\\a\\\"", "202- multiline response follows\r\n.\r\n");
  auto client = connected(console);
  auto empty = client.list("HDD:");
  REQUIRE_OK(empty);
  CHECK(empty->entries.empty());
  CHECK_EQ(empty->skipped, size_t{0});
  CHECK_OK(client.list("HDD:\\"));
  CHECK_OK(client.list("HDD:\\a"));
  CHECK_OK(client.list("HDD:\\a\\"));
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, ListingErrors) {
  auto console = FakeConsole::create();
  console->on("dirlist name=\"HDD:\\missing\\\"", "402- file not found\r\n")
      .on("dirlist name=\"HDD:\\secret\\\"", "414- access denied\r\n")
      .on("dirlist name=\"HDD:\\x\\\"", "200- OK\r\n");
  auto client = connected(console);
  auto missing = client.list("HDD:\\missing");
  REQUIRE_ERR(missing, ErrorCode::Io);
  expectStatus(missing.error(), 402);
  auto secret = client.list("HDD:\\secret");
  REQUIRE_ERR(secret, ErrorCode::Io);
  CHECK(secret.error().message.find("access denied") != std::string::npos);
  CHECK(client.isConnected());
  CHECK_ERR(client.list("HDD:\\x"), ErrorCode::Protocol);
  CHECK(!client.isConnected());
  CHECK_ERR(client.list("HDD:\\x"), ErrorCode::NotConnected);
}

TEST(XbdmClient, FileAttributes) {
  auto console = FakeConsole::create();
  console->on("getfileattributes name=\"HDD:\\a.bin\"",
              "202- multiline response follows\r\nsizehi=0x1 sizelo=0x2 createhi=0x01d11fb5 createlo=0x59683c00 "
              "changehi=0x01d11fb5 changelo=0x59683c00\r\n.\r\n")
      .on("getfileattributes name=\"HDD:\\dir\"", "200- sizehi=0x0 sizelo=0x0 directory\r\n")
      .on("getfileattributes name=\"HDD:\\none\"", "402- file not found\r\n")
      .on("getfileattributes name=\"HDD:\\bad\"", "200- sizehi=0xnope\r\n");
  auto client = connected(console);
  auto file = client.attributes("HDD:\\a.bin");
  REQUIRE_OK(file);
  CHECK_EQ(file->size, uint64_t{0x100000002});
  CHECK(!file->isDirectory);
  CHECK(file->changed().has_value());
  auto dir = client.attributes("HDD:\\dir");
  REQUIRE_OK(dir);
  CHECK(dir->isDirectory);
  auto none = client.attributes("HDD:\\none");
  REQUIRE(!none);
  expectStatus(none.error(), 402);
  CHECK_ERR(client.attributes("HDD:\\bad"), ErrorCode::Protocol);
  CHECK(client.isConnected());
  CHECK_EQ(console->problems(), std::string());
}

namespace {

// The intermediate name of a case-only rename, from the command that used it.
std::string intermediateIn(const std::string &command) {
  const auto at = command.find("newname=\"");
  if (at == std::string::npos) return {};
  return command.substr(at + 9, command.size() - at - 10);
}

bool isIntermediate(const std::string &path) {
  return path.rfind("HDD:\\A.txt.", 0) == 0 && path.size() > 4 && path.substr(path.size() - 4) == ".ren";
}

} // namespace

TEST(XbdmClient, RenameThatChangesOnlyCase) {
  auto console = FakeConsole::create();
  console->on("rename name=\"HDD:\\a.txt\" newname=\"HDD:\\A.txt\"", "200- OK\r\n");
  auto client = connected(console);
  CHECK_OK(client.rename("HDD:\\a.txt", "HDD:\\A.txt"));
  CHECK_EQ(console->commands().size(), size_t{1});
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, CaseOnlyRenameFallsBackToTwoSteps) {
  for (const std::string refusal : {"410- already exists", "400- unknown error"}) {
    auto console = FakeConsole::create();
    std::string intermediate;
    console->handle([&](FakeConsole &c, const std::string &line) {
      if (c.commands().size() == 1) {
        c.line(refusal);
      } else if (c.commands().size() == 2) {
        intermediate = intermediateIn(line);
        c.line("200- OK");
      } else {
        c.line("200- OK");
      }
    });
    auto client = connected(console);
    REQUIRE_OK(client.rename("HDD:\\a.txt", "HDD:\\A.txt"));
    REQUIRE_EQ(console->commands().size(), size_t{3});
    CHECK_MSG(isIntermediate(intermediate), intermediate);
    CHECK_EQ(console->commands()[1], "rename name=\"HDD:\\a.txt\" newname=\"" + intermediate + "\"");
    CHECK_EQ(console->commands()[2], "rename name=\"" + intermediate + "\" newname=\"HDD:\\A.txt\"");
  }
}

TEST(XbdmClient, ACaseOnlyRenameRefusedWith414IsReturned) {
  auto console = FakeConsole::create();
  console->on("rename name=\"HDD:\\a.txt\" newname=\"HDD:\\A.txt\"", "414- access denied\r\n");
  auto client = connected(console);
  auto r = client.rename("HDD:\\a.txt", "HDD:\\A.txt");
  REQUIRE_ERR(r, ErrorCode::Io);
  expectStatus(r.error(), 414);
  CHECK_EQ(console->commands().size(), size_t{1});
  CHECK(client.isConnected());
}

TEST(XbdmClient, ACaseOnlyRenameThatCannotFinishGoesBack) {
  auto console = FakeConsole::create();
  std::string intermediate;
  console->handle([&](FakeConsole &c, const std::string &line) {
    const size_t n = c.commands().size();
    if (n == 2) intermediate = intermediateIn(line);
    c.line(n == 1 || n == 3 ? "410- already exists" : "200- OK");
  });
  auto client = connected(console);
  auto r = client.rename("HDD:\\a.txt", "HDD:\\A.txt");
  REQUIRE_ERR(r, ErrorCode::Io);
  expectStatus(r.error(), 410);
  REQUIRE_EQ(console->commands().size(), size_t{4});
  CHECK_EQ(console->commands()[3], "rename name=\"" + intermediate + "\" newname=\"HDD:\\a.txt\"");
  CHECK_MSG(r.error().message.find("now named") == std::string::npos, r.error().message);

  // When going back fails too, the error says where the file is.
  auto stuck = FakeConsole::create();
  stuck->handle([&](FakeConsole &c, const std::string &line) {
    const size_t n = c.commands().size();
    if (n == 2) intermediate = intermediateIn(line);
    c.line(n == 2 ? "200- OK" : "410- already exists");
  });
  auto other = connected(stuck);
  auto lost = other.rename("HDD:\\a.txt", "HDD:\\A.txt");
  REQUIRE_ERR(lost, ErrorCode::Io);
  CHECK_EQ(stuck->commands().size(), size_t{4});
  CHECK_MSG(lost.error().message.find("; the file is now named " + intermediate) != std::string::npos,
            lost.error().message);
}

TEST(XbdmClient, RenameOntoItselfIsStillRefused) {
  auto console = FakeConsole::create();
  auto client = connected(console);
  CHECK_ERR(client.rename("HDD:\\a.txt", "HDD:\\a.txt"), ErrorCode::InvalidArgument);
  CHECK(console->commands().empty());
}

TEST(XbdmClient, MakeDirectoryDeleteAndRename) {
  auto console = FakeConsole::create();
  console->on("mkdir name=\"HDD:\\New Folder\"", "200- OK\r\n")
      .on("mkdir name=\"HDD:\\New Folder\"", "410- already exists\r\n")
      .on("delete name=\"HDD:\\a.txt\"", "200- OK\r\n")
      .on("delete name=\"HDD:\\dir\"", "414- access denied\r\n")
      .on("delete name=\"HDD:\\dir\" dir", "411- directory not empty\r\n")
      .on("delete name=\"HDD:\\empty\" dir", "200- OK\r\n")
      .on("getfileattributes name=\"HDD:\\b.txt\"", "402- file not found\r\n")
      .on("rename name=\"HDD:\\a.txt\" newname=\"HDD:\\b.txt\"", "200- OK\r\n")
      .on("getfileattributes name=\"HDD:\\sub\\c.txt\"", "402- file not found\r\n")
      .on("rename name=\"HDD:\\a.txt\" newname=\"HDD:\\sub\\c.txt\"", "409- must copy\r\n")
      .on("getfileattributes name=\"HDD:\\taken\"", "200- sizehi=0x0 sizelo=0x1\r\n");
  auto client = connected(console);
  CHECK_OK(client.makeDirectory("HDD:\\New Folder"));
  auto exists = client.makeDirectory("HDD:\\New Folder");
  REQUIRE(!exists);
  expectStatus(exists.error(), 410);
  CHECK_OK(client.removeFile("HDD:\\a.txt"));
  auto notFile = client.removeFile("HDD:\\dir");
  REQUIRE(!notFile);
  expectStatus(notFile.error(), 414);
  auto notEmpty = client.removeDirectory("HDD:\\dir");
  REQUIRE(!notEmpty);
  expectStatus(notEmpty.error(), 411);
  CHECK_OK(client.removeDirectory("HDD:\\empty"));
  CHECK_OK(client.rename("HDD:\\a.txt", "HDD:\\b.txt"));
  auto mustCopy = client.rename("HDD:\\a.txt", "HDD:\\sub\\c.txt");
  REQUIRE(!mustCopy);
  expectStatus(mustCopy.error(), 409);
  CHECK_ERR(client.rename("HDD:\\a.txt", "HDD:\\taken"), ErrorCode::AlreadyExists);
  CHECK_ERR(client.rename("HDD:\\a.txt", "DEVKIT:\\a.txt"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.makeDirectory("HDD:\\"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.removeFile("HDD:"), ErrorCode::InvalidArgument);
  CHECK(client.isConnected());
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, InvalidInputIsRefusedBeforeAnythingIsSent) {
  auto console = FakeConsole::create();
  auto client = connected(console);
  CHECK_ERR(client.removeFile("HDD:\\a\"b"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.makeDirectory("HDD:\\a\r\nmagicboot"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.list("/HDD/a"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.openRead("HDD:\\caf\xc3\xa9"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.moduleSections("a\"b"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.moduleSections(""), ErrorCode::InvalidArgument);
  CHECK_ERR(client.getMemory(0x80000000, 0), ErrorCode::InvalidArgument);
  CHECK_ERR(client.getMemory(0xFFFFFFF0, 0x20), ErrorCode::InvalidArgument);
  CHECK_ERR(client.getMemoryEx(0, kMaxMemoryReadBytes + 1), ErrorCode::LimitExceeded);
  CHECK_ERR(client.setMemory(0xFFFFFFFF, ut::Bytes{1, 2}), ErrorCode::InvalidArgument);
  CHECK_ERR(client.setMemory(0, ut::Bytes{}), ErrorCode::InvalidArgument);
  ClientOptions small = xt::quickOptions();
  small.maxCommandBytes = 32;
  client.setOptions(small);
  CHECK_ERR(client.list("HDD:\\a very long folder name here"), ErrorCode::LimitExceeded);
  CHECK(console->commands().empty());
  CHECK(client.isConnected());
}

TEST(XbdmClient, EveryRefusalCodeKeepsTheConnectionAndCarriesItsText) {
  const int codes[] = {400, 401, 402, 403, 404, 405, 406, 407, 408, 409, 410, 411, 412, 413, 414, 415,
                       416, 417, 418, 420, 421, 422, 423, 424, 425, 426, 427, 428, 429, 430, 431, 432,
                       437, 445, 446, 480, 481, 496, 497, 499};
  auto console = FakeConsole::create();
  for (int code : codes) {
    console->on("dvdeject", std::to_string(code) + "- reason " + std::to_string(code) + "\r\n");
  }
  console->on("dvdeject", "200- OK\r\n");
  auto client = connected(console);
  for (int code : codes) {
    auto r = client.ejectTray();
    REQUIRE(!r);
    expectStatus(r.error(), code);
    CHECK(r.error().message.find("reason " + std::to_string(code)) != std::string::npos);
    const ErrorCode expected = code == 401 || code == 406 || code == 446 ? ErrorCode::LimitExceeded
                               : code == 407                             ? ErrorCode::Unsupported
                                                                         : ErrorCode::Io;
    CHECK_EQ(r.error().code, expected);
    CHECK(client.isConnected());
    REQUIRE(client.lastStatus().has_value());
    CHECK_EQ(client.lastStatus()->code, code);
  }
  CHECK_OK(client.ejectTray());
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, UnknownStatusCodesDropTheConnectionWithTheRawText) {
  for (const char *answer : {"299- what\r\n", "500- internal\r\n", "100- continue\r\n", "300- moved\r\n",
                             "abc\r\n", "-\r\n", "20- short\r\n", "\r\n", "203- binary response follows\r\n",
                             "202- multiline response follows\r\n.\r\n", "201- connected\r\n"}) {
    auto console = FakeConsole::create();
    console->on("dvdeject", answer);
    auto client = connected(console);
    auto r = client.ejectTray();
    REQUIRE_ERR(r, ErrorCode::Protocol);
    CHECK(!consoleStatusCode(r.error()).has_value());
    CHECK(!client.isConnected());
    CHECK(console->closed());
  }
  auto console = FakeConsole::create();
  console->on("dvdeject", "500- the \x01 thing\r\n");
  auto client = connected(console);
  auto r = client.ejectTray();
  REQUIRE(!r);
  CHECK_MSG(r.error().message.find("500- the \\x01 thing") != std::string::npos, r.error().message);
}

TEST(XbdmClient, ConsoleStatusCodeIgnoresOtherErrors) {
  CHECK(!consoleStatusCode(makeError(ErrorCode::Io, "read failed", 5)).has_value());
  CHECK(!consoleStatusCode(makeError(ErrorCode::Io, "x", 410)).has_value());
  CHECK(!consoleStatusCode(makeError(ErrorCode::Io, "console answered 411- x", 410)).has_value());
  CHECK_EQ(consoleStatusCode(makeError(ErrorCode::Io, "y: console answered 410- x", 410)).value_or(0), 410);
}

TEST(XbdmClient, PowerCommandsAcceptAnAnswerOrAClosedConnection) {
  {
    auto console = FakeConsole::create();
    console->on("magicboot", "200- OK\r\n");
    auto client = connected(console);
    auto r = client.reboot();
    REQUIRE_OK(r);
    CHECK(*r == PowerResult::Acknowledged);
    CHECK(!client.isConnected());
    CHECK_EQ(console->byes(), 0);
  }
  {
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &c, const std::string &line) {
      if (line == "magicboot cold") c.hangUp();
    });
    auto client = connected(console);
    auto r = client.reboot(RebootMode::Cold);
    REQUIRE_OK(r);
    CHECK(*r == PowerResult::ConnectionClosed);
    CHECK_EQ(console->lastCommand(), std::string("magicboot cold"));
  }
  {
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &c, const std::string &line) {
      if (line == "shutdown") c.hangUp();
    });
    auto client = connected(console);
    CHECK(client.shutdown().value_or(PowerResult::Acknowledged) == PowerResult::ConnectionClosed);
  }
  {
    auto console = FakeConsole::create();
    console->on("magicboot title=\"HDD:\\Games\\Game\\default.xex\" directory=\"HDD:\\Games\\Game\"", "200- OK\r\n")
        .on("magicboot title=\"HDD:\\default.xex\" directory=\"HDD:\\\"", "200- OK\r\n");
    auto client = connected(console);
    CHECK_OK(client.launch("HDD:\\Games\\Game\\default.xex"));
    CHECK_ERR(client.launch("HDD:\\default.xex"), ErrorCode::NotConnected);
    CHECK_ERR(client.reconnect(), ErrorCode::Unsupported);
  }
  {
    auto console = FakeConsole::create();
    console->on("magicboot title=\"HDD:\\default.xex\" directory=\"HDD:\\\"", "200- OK\r\n");
    auto client = connected(console);
    CHECK_OK(client.launch("HDD:\\default.xex"));
  }
  {
    auto console = FakeConsole::create();
    console->on("magicboot", "408- not stopped\r\n");
    auto client = connected(console);
    auto r = client.reboot();
    REQUIRE(!r);
    expectStatus(r.error(), 408);
    CHECK(client.isConnected());
  }
  {
    auto console = FakeConsole::create();
    console->on("magicboot", "200");
    console->hangUp();
    auto client = connected(console);
    CHECK_ERR(client.reboot(), ErrorCode::Disconnected);
  }
  {
    auto console = FakeConsole::create();
    console->on("shutdown", "");
    auto client = connected(console);
    CHECK_ERR(client.shutdown(), ErrorCode::Timeout);
    CHECK(!client.isConnected());
  }
  {
    auto console = FakeConsole::create();
    console->on("dvdeject", "200- OK\r\n");
    auto client = connected(console);
    CHECK_OK(client.ejectTray());
    CHECK(client.isConnected());
  }
}

TEST(XbdmClient, Screenshot) {
  const ut::Bytes pixels = ut::patternBytes(0x80 * 32, 3);
  auto console = FakeConsole::create();
  console->handle([&](FakeConsole &c, const std::string &line) {
    if (line != "screenshot") return;
    c.line("203- binary response follows");
    c.line("pitch=0x00000080 width=0x00000020 height=0x00000020 format=0x1a8 offsetx=0x0 offsety=0x0, "
           "framebuffersize=0x00001000");
    c.send(pixels);
  });
  auto client = connected(console);
  auto shot = client.screenshot();
  REQUIRE_OK(shot);
  CHECK_EQ(shot->pitch, uint32_t{0x80});
  CHECK_EQ(shot->width, uint32_t{32});
  CHECK_EQ(shot->height, uint32_t{32});
  CHECK_EQ(shot->format, uint32_t{0x1a8});
  CHECK_EQ(shot->data, pixels);
  CHECK(client.isConnected());
  CHECK_EQ(console->lastTimeout(), std::chrono::milliseconds(300));
}

TEST(XbdmClient, ScreenshotGeometryIsChecked) {
  const char *lines[] = {
      "pitch=0x80 width=0x20 height=0x20 format=0x1 framebuffersize=0x1001",   // above pitch * height
      "pitch=0x80 width=0x20 format=0x1 framebuffersize=0x10",                 // no height
      "pitch=0xzz width=0x20 height=0x20 format=0x1 framebuffersize=0x10",     // junk
      "pitch=0x10000 width=0x20 height=0x10000 format=0x1 framebuffersize=0xffffffff", // above 64 MiB
  };
  for (const char *geometry : lines) {
    auto console = FakeConsole::create();
    console->handle([&](FakeConsole &c, const std::string &) {
      c.line("203- binary response follows");
      c.line(geometry);
    });
    auto client = connected(console);
    auto r = client.screenshot();
    CHECK(!r);
    CHECK(!client.isConnected());
  }
  auto console = FakeConsole::create();
  console->handle([&](FakeConsole &c, const std::string &) {
    c.line("203- binary response follows");
    c.line("pitch=0x80 width=0x20 height=0x1 format=0x1 framebuffersize=0x1000");
  });
  auto client = connected(console);
  CHECK_ERR(client.screenshot(), ErrorCode::Timeout);
}

TEST(XbdmClient, SetSystemTime) {
  auto console = FakeConsole::create();
  console->on("setsystime clockhi=0x1d11fb5 clocklo=0x59683c00", "200- OK\r\n")
      .on("setsystime clockhi=0x0 clocklo=0x0", "200- OK\r\n")
      .on("setsystime clockhi=0x1d11fb5 clocklo=0x59683c00", "406- clock not set\r\n");
  auto client = connected(console);
  auto when = fileTimeToTimePoint(0x01d11fb559683c00);
  REQUIRE(when.has_value());
  CHECK_OK(client.setSystemTime(*when));
  CHECK_OK(client.setSystemTimeRaw(0));
  auto r = client.setSystemTimeRaw(0x01d11fb559683c00);
  REQUIRE_ERR(r, ErrorCode::LimitExceeded);
  expectStatus(r.error(), 406);
  CHECK_ERR(client.setSystemTime(*fileTimeToTimePoint(0) - FileTimeTicks(1)), ErrorCode::InvalidArgument);
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, GetMemTextWithUnreadableBytes) {
  auto console = FakeConsole::create();
  console->on("getmem addr=0x82000000 length=0x8", "202- multiline response follows\r\n4D5a90\r\n00????ff00\r\n.\r\n")
      .on("getmem addr=0x82000000 length=0x4", "202- multiline response follows\r\n4D5A\r\n.\r\n")
      .on("getmem addr=0x82000000 length=0x2", "202- multiline response follows\r\n4D5A90\r\n.\r\n");
  auto client = connected(console);
  auto read = client.getMemory(0x82000000, 8);
  REQUIRE_OK(read);
  CHECK_EQ(read->data, (ut::Bytes{0x4D, 0x5A, 0x90, 0x00, 0x00, 0x00, 0xFF, 0x00}));
  CHECK_EQ(read->readableBytes(), size_t{6});
  REQUIRE_EQ(read->readable.size(), size_t{8});
  CHECK(!read->readable[4]);
  CHECK(!read->readable[5]);
  CHECK(read->readable[6]);
  CHECK_ERR(client.getMemory(0x82000000, 4), ErrorCode::Protocol);
  CHECK(client.isConnected());
  CHECK_ERR(client.getMemory(0x82000000, 2), ErrorCode::Protocol);
  CHECK(!client.isConnected());
}

TEST(XbdmClient, GetMemTextAsksForAtMost0x400BytesAtATime) {
  auto console = FakeConsole::create();
  auto hexOf = [](size_t bytes, char digit) { return std::string(bytes * 2, digit); };
  console->on("getmem addr=0x82000000 length=0x400", "202- multiline response follows\r\n" + hexOf(0x400, 'a') + "\r\n.\r\n")
      .on("getmem addr=0x82000400 length=0x400",
          "202- multiline response follows\r\n" + hexOf(0x200, 'b') + "\r\n" + hexOf(0x200, '?') + "\r\n.\r\n")
      .on("getmem addr=0x82000800 length=0x101", "202- multiline response follows\r\n" + hexOf(0x101, 'c') + "\r\n.\r\n")
      .on("getmem addr=0x0 length=0x400", "202- multiline response follows\r\n" + hexOf(0x400, '0') + "\r\n.\r\n")
      .on("getmem addr=0x400 length=0x10", "404- memory not mapped\r\n");
  auto client = connected(console);
  auto read = client.getMemory(0x82000000, 0x901);
  REQUIRE_OK(read);
  REQUIRE_EQ(read->data.size(), size_t{0x901});
  CHECK_EQ(read->data[0], uint8_t{0xaa});
  CHECK_EQ(read->data[0x400], uint8_t{0xbb});
  CHECK(!read->readable[0x600]);
  CHECK_EQ(read->data[0x900], uint8_t{0xcc});
  CHECK_EQ(read->readableBytes(), size_t{0x901 - 0x200});
  // A refusal of a later piece fails the whole read.
  auto refused = client.getMemory(0, 0x410);
  REQUIRE(!refused);
  expectStatus(refused.error(), 404);
  CHECK(client.isConnected());
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, GetMemTextRefusalAndJunk) {
  auto console = FakeConsole::create();
  console->on("getmem addr=0x0 length=0x1", "404- memory not mapped\r\n")
      .on("getmem addr=0x0 length=0x2", "202- multiline response follows\r\nzz00\r\n.\r\n");
  auto client = connected(console);
  auto r = client.getMemory(0, 1);
  REQUIRE(!r);
  expectStatus(r.error(), 404);
  CHECK_ERR(client.getMemory(0, 2), ErrorCode::Protocol);
  CHECK(!client.isConnected());
}

TEST(XbdmClient, GetMemExBlocks) {
  const ut::Bytes first = ut::patternBytes(0x400, 1);
  const ut::Bytes second = ut::patternBytes(0x100, 2);
  auto console = FakeConsole::create();
  console->handle([&](FakeConsole &c, const std::string &line) {
    if (line == "getmemex addr=0x82000000 length=0x500") {
      c.line("203- binary response follows");
      c.send(ut::Bytes{0x00, 0x04});
      c.send(first);
      c.send(ut::Bytes{0x00, 0x81});
      c.send(second);
    } else if (line == "getmemex addr=0x90000000 length=0x10") {
      c.line("203- binary response follows");
      c.send(ut::Bytes{0x04, 0x00, 1, 2, 3, 4});
      c.send(ut::Bytes{0x04, 0x00, 5, 6, 7, 8});
      c.send(ut::Bytes{0x00, 0x80});
    } else if (line == "getmemex addr=0x0 length=0x4") {
      c.line("203- binary response follows");
      c.send(ut::Bytes{0x04, 0x00, 9, 9, 9, 9});
    }
  });
  auto client = connected(console);
  auto full = client.getMemoryEx(0x82000000, 0x500);
  REQUIRE_OK(full);
  CHECK_EQ(full->readableBytes(), size_t{0x500});
  CHECK(std::equal(first.begin(), first.end(), full->data.begin()));
  CHECK(std::equal(second.begin(), second.end(), full->data.begin() + 0x400));

  auto partial = client.getMemoryEx(0x90000000, 0x10);
  REQUIRE_OK(partial);
  CHECK_EQ(partial->readableBytes(), size_t{8});
  CHECK(partial->readable[7]);
  CHECK(!partial->readable[8]);
  CHECK_EQ(partial->data[8], uint8_t{0});
  CHECK_EQ(partial->data[7], uint8_t{8});

  auto noLastBit = client.getMemoryEx(0, 4);
  REQUIRE_OK(noLastBit);
  CHECK_EQ(noLastBit->readableBytes(), size_t{4});
  CHECK(client.isConnected());
}

TEST(XbdmClient, GetMemExRefusesBadBlocks) {
  auto big = FakeConsole::create();
  big->handle([](FakeConsole &c, const std::string &) {
    c.line("203- binary response follows");
    c.send(ut::Bytes{0x05, 0x00, 1, 2, 3, 4, 5});
  });
  auto client = connected(big);
  CHECK_ERR(client.getMemoryEx(0, 4), ErrorCode::Protocol);
  CHECK(!client.isConnected());

  auto empty = FakeConsole::create();
  empty->handle([](FakeConsole &c, const std::string &) {
    c.line("203- binary response follows");
    c.send(ut::Bytes{0x00, 0x00, 0x00, 0x00});
  });
  auto other = connected(empty);
  CHECK_ERR(other.getMemoryEx(0, 4), ErrorCode::Protocol);

  auto refused = FakeConsole::create();
  refused->on("getmemex addr=0x0 length=0x4", "407- unknown command\r\n");
  auto third = connected(refused);
  auto r = third.getMemoryEx(0, 4);
  REQUIRE_ERR(r, ErrorCode::Unsupported);
  expectStatus(r.error(), 407);
  CHECK(third.isConnected());
}

TEST(XbdmClient, SetMemSendsPiecesOf64Bytes) {
  const ut::Bytes data = ut::patternBytes(130, 5);
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &line) {
    if (line.rfind("setmem addr=0x82000080 ", 0) == 0) {
      c.line("404- memory not mapped");
    } else {
      c.line("200- set bytes");
    }
  });
  auto client = connected(console);
  CHECK_OK(client.setMemory(0x82000000, std::span<const uint8_t>(data).first(128)));
  REQUIRE_EQ(console->commands().size(), size_t{2});
  CHECK_EQ(console->commands()[0], "setmem addr=0x82000000 data=" + formatHex(std::span<const uint8_t>(data).first(64)));
  CHECK_EQ(console->commands()[1],
           "setmem addr=0x82000040 data=" + formatHex(std::span<const uint8_t>(data).subspan(64, 64)));

  auto r = client.setMemory(0x82000000, data);
  REQUIRE(!r);
  expectStatus(r.error(), 404);
  CHECK_EQ(console->commands().size(), size_t{5});
  CHECK(client.isConnected());
}

TEST(XbdmClient, MemoryRegionsModulesAndSections) {
  auto console = FakeConsole::create();
  console->on("walkmem", "202- multiline response follows\r\nbase=0x82000000 size=0x00a40000 protect=0x00000004 "
                         "phys=0x00000000\r\nbase=0x90000000 size=0x1000\r\nbase=junk size=0x1\r\n.\r\n")
      .on("modules", "202- multiline response follows\r\nname=\"xboxkrnl.exe\" base=0x80040000 size=0x001a0000 "
                     "check=0x00000000 timestamp=0x4c1a2b3c tls xbe\r\nname=\"dash.xex\" base=0x92000000 "
                     "size=0x100 osize=0x200\r\nbase=0x1 size=0x2\r\n.\r\n")
      .on("modsections name=\"dash.xex\"", "202- multiline response follows\r\nname=\".text\" base=0x92000000 "
                                           "size=0x80 index=0x0 flags=0x1\r\nname=\".data\" base=0x92000080 "
                                           "size=0x80\r\n.\r\n")
      .on("modsections name=\"nope.xex\"", "402- no such file\r\n");
  auto client = connected(console);
  auto regions = client.memoryRegions();
  REQUIRE_OK(regions);
  REQUIRE_EQ(regions->size(), size_t{2});
  CHECK_EQ((*regions)[0].size, uint32_t{0x00a40000});
  CHECK_EQ((*regions)[0].protect, uint32_t{4});
  CHECK_EQ((*regions)[1].protect, uint32_t{0});

  auto modules = client.modules();
  REQUIRE_OK(modules);
  REQUIRE_EQ(modules->size(), size_t{2});
  CHECK_EQ((*modules)[0].name, std::string("xboxkrnl.exe"));
  CHECK_EQ((*modules)[0].timestamp.value_or(0), uint32_t{0x4c1a2b3c});
  CHECK_EQ((*modules)[1].originalSize.value_or(0), uint32_t{0x200});
  CHECK(!(*modules)[1].checksum.has_value());

  auto sections = client.moduleSections("dash.xex");
  REQUIRE_OK(sections);
  REQUIRE_EQ(sections->size(), size_t{2});
  CHECK_EQ((*sections)[0].name, std::string(".text"));
  CHECK_EQ((*sections)[0].flags.value_or(0), uint32_t{1});
  CHECK(!(*sections)[1].index.has_value());

  auto missing = client.moduleSections("nope.xex");
  REQUIRE(!missing);
  expectStatus(missing.error(), 402);
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmClient, XbdmSchemeDefaultsToPort730) {
  net::TransportRegistry registry;
  registerXbdmScheme(registry);
  CHECK_EQ(registry.schemes(), std::vector<std::string>{"xbdm"});
  auto endpoint = net::Endpoint::parse("xbdm://192.168.1.20");
  REQUIRE_OK(endpoint);
  CHECK_EQ(registry.withDefaultPort(*endpoint, 49).port, 730);
  auto explicitPort = net::Endpoint::parse("xbdm://192.168.1.20:731");
  REQUIRE_OK(explicitPort);
  CHECK_EQ(registry.withDefaultPort(*explicitPort, 49).port, 731);
}

namespace {

bool sameDelivery(const std::optional<CommandDelivery> &got, const std::string &command, Delivery delivery) {
  return got && got->command == command && got->delivery == delivery;
}

} // namespace

TEST(XbdmClient, LastDeliveryTellsHowFarAFailedCommandGot) {
  {
    auto console = FakeConsole::create();
    auto client = connected(console);
    CHECK(!client.lastDelivery().has_value());
    client.close();
    auto r = client.removeFile("HDD:\\a.txt");
    REQUIRE_ERR(r, ErrorCode::NotConnected);
    CHECK(sameDelivery(client.lastDelivery(), "delete", Delivery::NotSent));
    CHECK_MSG(r.error().message.find("; the command was not sent") != std::string::npos, r.error().message);
  }
  {
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &c, const std::string &) { c.hangUp(); });
    auto client = connected(console);
    auto r = client.removeFile("HDD:\\a.txt");
    REQUIRE_ERR(r, ErrorCode::Disconnected);
    CHECK(sameDelivery(client.lastDelivery(), "delete", Delivery::Sent));
    CHECK_MSG(r.error().message.find("sent but not answered, so the console may have carried it out") !=
                  std::string::npos,
              r.error().message);
  }
  {
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &, const std::string &) {});
    auto client = connected(console);
    auto r = client.removeFile("HDD:\\a.txt");
    REQUIRE_ERR(r, ErrorCode::Timeout);
    CHECK(sameDelivery(client.lastDelivery(), "delete", Delivery::Sent));
  }
  {
    auto console = FakeConsole::create();
    console->on("delete name=\"HDD:\\a.txt\"", "414- access denied\r\n");
    auto client = connected(console);
    auto r = client.removeFile("HDD:\\a.txt");
    REQUIRE_ERR(r, ErrorCode::Io);
    expectStatus(r.error(), 414);
    CHECK(sameDelivery(client.lastDelivery(), "delete", Delivery::Answered));
    CHECK_MSG(r.error().message.find("the command was") == std::string::npos, r.error().message);
  }
  {
    auto console = FakeConsole::create();
    console->on("delete name=\"HDD:\\a.txt\"", "200- OK\r\n");
    auto client = connected(console);
    REQUIRE_OK(client.removeFile("HDD:\\a.txt"));
    CHECK(sameDelivery(client.lastDelivery(), "delete", Delivery::Answered));
    // A call refused before any command keeps the last delivery.
    CHECK_ERR(client.removeFile("HDD:"), ErrorCode::InvalidArgument);
    CHECK(sameDelivery(client.lastDelivery(), "delete", Delivery::Answered));
  }
}

TEST(XbdmClient, ARenameWhoseCheckFailsWasNotSent) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &) { c.hangUp(); });
  auto client = connected(console);
  auto r = client.rename("HDD:\\a.txt", "HDD:\\b.txt");
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK(sameDelivery(client.lastDelivery(), "getfileattributes", Delivery::Sent));
  CHECK_MSG(r.error().message.find("; the command was not sent") != std::string::npos, r.error().message);
}

TEST(XbdmClient, SetMemorySaysHowMuchWasWrittenBeforeAFailure) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &) {
    if (c.commands().size() == 3) {
      c.hangUp();
      return;
    }
    c.line("200- set");
  });
  auto client = connected(console);
  const ut::Bytes data(200, 0xAB);
  auto r = client.setMemory(0x82000000u, data);
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK_EQ(console->commands().size(), size_t{3});
  CHECK_MSG(r.error().message.find("the first 128 bytes were written") != std::string::npos, r.error().message);
  CHECK(sameDelivery(client.lastDelivery(), "setmem", Delivery::Sent));
}

TEST(XbdmClient, ACaseOnlyRenameThatStopsAfterTheIntermediateIsNotNotSent) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &) {
    if (c.commands().size() == 1) {
      c.line("410- already exists");
      return;
    }
    // The move to the intermediate name is answered; then the connection goes
    // before a byte of the last step leaves.
    c.line("200- OK");
    c.dropAfterWritten(c.written());
  });
  auto client = connected(console);
  auto r = client.rename("HDD:\\a.txt", "HDD:\\A.txt");
  REQUIRE(!r);
  CHECK_EQ(console->commands().size(), size_t{2});
  CHECK(sameDelivery(client.lastDelivery(), "rename", Delivery::Sent));
  CHECK_MSG(r.error().message.find("not sent") == std::string::npos, r.error().message);
  CHECK_MSG(r.error().message.find("an earlier step was carried out") != std::string::npos, r.error().message);
}

TEST(XbdmClient, SetMemoryIsNotSentOnlyWhenNoPieceWent) {
  {
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &c, const std::string &) {
      c.line("200- set");
      if (c.commands().size() == 2) c.dropAfterWritten(c.written());
    });
    auto client = connected(console);
    REQUIRE(!client.setMemory(0x82000000u, ut::Bytes(200, 0xAB)));
    CHECK_EQ(console->commands().size(), size_t{2});
    CHECK(sameDelivery(client.lastDelivery(), "setmem", Delivery::Sent));
  }
  {
    auto console = FakeConsole::create();
    auto client = connected(console);
    console->dropAfterWritten(console->written());
    REQUIRE(!client.setMemory(0x82000000u, ut::Bytes(200, 0xAB)));
    CHECK(console->commands().empty());
    CHECK(sameDelivery(client.lastDelivery(), "setmem", Delivery::NotSent));
  }
}

TEST(XbdmClient, AReadOnlyCallAfterAChangeStartsAfresh) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &) {
    c.line("200- OK");
    if (c.commands().size() == 1) c.dropAfterWritten(c.written());
  });
  auto client = connected(console);
  REQUIRE_OK(client.makeDirectory("HDD:\\d"));
  // A new call that fails before it sends anything is NotSent again.
  CHECK(!client.removeFile("HDD:\\d"));
  CHECK(sameDelivery(client.lastDelivery(), "delete", Delivery::NotSent));
}

TEST(XbdmClient, ACaseOnlyRenameThatDoesNotFinishSaysWhereTheFileIs) {
  // What the console does to the steps after the first rename's 410: "ok" answers,
  // "refuse" answers 410, "hang" closes without an answer, "cut" answers and closes
  // before the next line can leave.
  struct Case {
    std::vector<std::string> steps;
    std::string where; // with S, I and T for source, intermediate and target
  };
  const std::vector<Case> cases = {
      {{"refuse"}, "still named S"},
      {{"cut-first"}, "still named S"},
      {{"hang"}, "named S or I"},
      {{"cut"}, "now named I"},
      {{"ok", "hang"}, "named I or T"},
      {{"ok", "refuse", "ok"}, "still named S"},
      {{"ok", "refuse", "refuse"}, "now named I"},
      {{"ok", "refuse", "hang"}, "named I or S"},
  };
  for (const auto &test : cases) {
    auto console = FakeConsole::create();
    std::string intermediate;
    console->handle([&](FakeConsole &c, const std::string &line) {
      const size_t n = c.commands().size();
      if (n == 1) {
        c.line("410- already exists");
        if (test.steps[0] == "cut-first") c.dropAfterWritten(c.written());
        return;
      }
      if (n == 2) intermediate = intermediateIn(line);
      const std::string &step = test.steps[n - 2];
      if (step == "refuse") {
        c.line("410- already exists");
      } else if (step == "hang") {
        c.hangUp();
      } else {
        c.line("200- OK");
        if (step == "cut") c.dropAfterWritten(c.written());
      }
    });
    auto client = connected(console);
    auto r = client.rename("HDD:\\a.txt", "HDD:\\A.txt");
    REQUIRE(!r);
    std::string where = "; the file is " + test.where;
    auto put = [&](const std::string &key, const std::string &value) {
      for (size_t at = where.find(key); at != std::string::npos; at = where.find(key, at + value.size())) {
        where.replace(at, key.size(), value);
      }
    };
    put(" S", " HDD:\\a.txt");
    put(" I", " " + intermediate);
    put(" T", " HDD:\\A.txt");
    CHECK_MSG(r.error().message.find(where) != std::string::npos, where + " in: " + r.error().message);
  }
}

TEST(XbdmClient, RenameComparesDrivesWithoutCase) {
  auto console = FakeConsole::create();
  console->on("getfileattributes name=\"HDD:\\b.txt\"", "402- file not found\r\n")
      .on("rename name=\"hdd:\\a.txt\" newname=\"HDD:\\b.txt\"", "200- OK\r\n");
  auto client = connected(console);
  CHECK_OK(client.rename("hdd:\\a.txt", "HDD:\\b.txt"));
  CHECK_EQ(console->problems(), std::string());
}
