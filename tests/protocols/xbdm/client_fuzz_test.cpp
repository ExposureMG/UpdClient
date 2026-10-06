#include "protocols/xbdm/client_fake.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <updclient/protocols/xbdm/client.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

// Random and mutated console output for every command. The client must neither
// crash nor hang, must keep its invariants on success, and must leave the
// connection either usable or closed. Bounded by time, so it suits sanitizer runs.

using namespace updclient;
using namespace updclient::xbdm;
using xt::FakeConsole;

namespace {

using Clock = std::chrono::steady_clock;

struct Op {
  const char *name;
  std::function<void(XbdmClient &)> run;
};

ut::Bytes le16(uint16_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
}

ut::Bytes le32(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

// Well-formed answers the mutations start from.
std::vector<ut::Bytes> corpus() {
  std::vector<ut::Bytes> out;
  auto add = [&](std::string_view text) { out.push_back(ut::bytesOf(text)); };
  add("200- OK\r\n");
  add("200- Box Name\r\n");
  add("200- consoleid=0123456789AB\r\n");
  add("200- addr=0xc0a80102\r\n");
  add("200- start\r\n");
  add("200- freetocallerhi=0x0 freetocallerlo=0x10 totalbyteshi=0x0 totalbyteslo=0x100\r\n");
  add("200- sizehi=0x0 sizelo=0x10 createhi=0x1 createlo=0x2 changehi=0x3 changelo=0x4 directory\r\n");
  add("202- multiline response follows\r\n.\r\n");
  add("202- multiline response follows\r\ndrivename=\"HDD\"\r\ndrivename=\"DEVKIT\"\r\n.\r\n");
  add("202- multiline response follows\r\nname=\"a b\" sizehi=0x0 sizelo=0x1 createhi=0x01d11fb5 createlo=0x59683c00 "
      "changehi=0x01d11fb5 changelo=0x59683c00 directory\r\nname=\"c\" sizelo=0x2\r\n.\r\n");
  add("202- multiline response follows\r\nname=\"dash.xex\" timestamp=0x1 checksum=0x2\r\n.\r\n");
  add("202- multiline response follows\r\n4D5A9000????????4D5A9000????????4D5A9000????????4D5A9000????????"
      "4D5A9000????????4D5A9000????????4D5A9000????????4D5A9000????????\r\n.\r\n");
  add("202- multiline response follows\r\nbase=0x82000000 size=0x1000 protect=0x4 phys=0x0\r\n.\r\n");
  add("202- multiline response follows\r\nname=\"m.xex\" base=0x1 size=0x2 check=0x3 timestamp=0x4 osize=0x5\r\n"
      "name=\".text\" base=0x1 size=0x2 index=0x0 flags=0x1\r\n.\r\n");
  add("204- send binary data\r\n");
  add("401- max number of connections exceeded\r\n");
  add("410- already exists\r\n");
  add("500- what\r\n");

  ut::Bytes file = ut::bytesOf("203- binary response follows\r\n");
  ut::append(file, le32(20));
  ut::append(file, ut::patternBytes(20, 1));
  out.push_back(file);

  ut::Bytes memory = ut::bytesOf("203- binary response follows\r\n");
  ut::append(memory, le16(0x20));
  ut::append(memory, ut::patternBytes(0x20, 2));
  ut::append(memory, le16(0x8020));
  ut::append(memory, ut::patternBytes(0x20, 3));
  out.push_back(memory);

  ut::Bytes shot = ut::bytesOf("203- binary response follows\r\n"
                               "pitch=0x4 width=0x1 height=0x1 format=0x0 offsetx=0x0, offsety=0x0 "
                               "framebuffersize=0x4\r\n");
  ut::append(shot, ut::Bytes{1, 2, 3, 4});
  out.push_back(shot);
  return out;
}

class Fuzzer {
public:
  explicit Fuzzer(uint32_t seed) : rng_(seed), corpus_(corpus()) {}

  size_t below(size_t n) { return n == 0 ? 0 : std::uniform_int_distribution<size_t>(0, n - 1)(rng_); }
  bool chance(int percent) { return static_cast<int>(below(100)) < percent; }
  uint8_t byte() { return static_cast<uint8_t>(below(256)); }

  ut::Bytes answer() {
    if (chance(15)) {
      ut::Bytes noise(below(300));
      for (auto &b : noise) b = chance(10) ? static_cast<uint8_t>("\r\n.-\"= 0x2"[below(10)]) : byte();
      return noise;
    }
    ut::Bytes out = corpus_[below(corpus_.size())];
    const size_t mutations = below(4);
    for (size_t m = 0; m < mutations && !out.empty(); ++m) mutate(out);
    if (chance(10)) ut::append(out, corpus_[below(corpus_.size())]);
    return out;
  }

  ut::Bytes greeting() {
    if (chance(90)) return ut::bytesOf("201- connected\r\n");
    ut::Bytes out = ut::bytesOf("201- connected\r\n");
    mutate(out);
    return out;
  }

private:
  void mutate(ut::Bytes &out) {
    const size_t at = below(out.size());
    switch (below(7)) {
    case 0: out[at] = byte(); break;
    case 1: out[at] ^= static_cast<uint8_t>(1u << below(8)); break;
    case 2: out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), byte()); break;
    case 3: out.erase(out.begin() + static_cast<std::ptrdiff_t>(at),
                      out.begin() + static_cast<std::ptrdiff_t>(std::min(out.size(), at + 1 + below(8))));
      break;
    case 4: out.resize(at); break;
    case 5: {
      const ut::Bytes piece(out.begin() + static_cast<std::ptrdiff_t>(at), out.end());
      out.insert(out.end(), piece.begin(), piece.end());
      break;
    }
    case 6: {
      static const char *digits = "0123456789abcdefFqx";
      if (out[at] >= '0' && out[at] <= '9') out[at] = static_cast<uint8_t>(digits[below(19)]);
      else out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), {'\r', '\n'});
      break;
    }
    }
  }

  std::mt19937 rng_;
  std::vector<ut::Bytes> corpus_;
};

std::vector<Op> operations() {
  return {
      {"dbgname", [](XbdmClient &c) { (void)c.debugName(); }},
      {"consoletype", [](XbdmClient &c) { (void)c.consoleType(); }},
      {"getconsoleid", [](XbdmClient &c) { (void)c.consoleId(); }},
      {"xbeinfo", [](XbdmClient &c) { (void)c.runningTitle(); }},
      {"getexecstate", [](XbdmClient &c) { (void)c.execState(); }},
      {"altaddr", [](XbdmClient &c) { (void)c.titleAddress(); }},
      {"consoleinfo", [](XbdmClient &c) { (void)c.consoleInfo(); }},
      {"drivelist",
       [](XbdmClient &c) {
         auto r = c.drives();
         if (r) {
           for (const auto &name : *r) CHECK_OK(validateDriveName(name));
         }
       }},
      {"drivefreespace", [](XbdmClient &c) { (void)c.driveSpace("HDD"); }},
      {"dirlist",
       [](XbdmClient &c) {
         auto r = c.list("HDD:\\dir");
         if (!r) return;
         for (const auto &entry : r->entries) {
           CHECK(!entry.name.empty());
           CHECK(entry.name != "." && entry.name != "..");
           CHECK(entry.name.find('\\') == std::string::npos);
         }
       }},
      {"getfileattributes", [](XbdmClient &c) { (void)c.attributes("HDD:\\f"); }},
      {"mkdir", [](XbdmClient &c) { (void)c.makeDirectory("HDD:\\d"); }},
      {"delete", [](XbdmClient &c) { (void)c.removeFile("HDD:\\f"); }},
      {"rmdir", [](XbdmClient &c) { (void)c.removeDirectory("HDD:\\d"); }},
      {"rename", [](XbdmClient &c) { (void)c.rename("HDD:\\a", "HDD:\\b"); }},
      {"getfile",
       [](XbdmClient &c) {
         auto reader = c.openRead("HDD:\\f");
         if (!reader) return;
         std::vector<uint8_t> buffer(13);
         uint64_t total = 0;
         for (int i = 0; i < 100000; ++i) {
           auto n = reader->read(buffer);
           if (!n || *n == 0) break;
           total += *n;
         }
         CHECK(total <= reader->size());
         CHECK(!c.transferActive() || reader->isOpen());
       }},
      {"sendfile",
       [](XbdmClient &c) {
         auto writer = c.openWrite("HDD:\\up.bin", 100);
         if (!writer) return;
         if (writer->write(ut::patternBytes(100, 4))) (void)writer->finish();
         CHECK(!writer->isOpen());
         CHECK(!c.transferActive());
       }},
      {"magicboot", [](XbdmClient &c) { (void)c.reboot(); }},
      {"launch", [](XbdmClient &c) { (void)c.launch("HDD:\\default.xex"); }},
      {"shutdown", [](XbdmClient &c) { (void)c.shutdown(); }},
      {"dvdeject", [](XbdmClient &c) { (void)c.ejectTray(); }},
      {"screenshot",
       [](XbdmClient &c) {
         auto r = c.screenshot();
         if (r) CHECK(r->data.size() <= uint64_t{r->pitch} * ((uint64_t{r->height} + 31) & ~uint64_t{31}));
       }},
      {"setsystime", [](XbdmClient &c) { (void)c.setSystemTimeRaw(0x01d11fb559683c00); }},
      {"getmem",
       [](XbdmClient &c) {
         auto r = c.getMemory(0x82000000, 64);
         if (r) {
           CHECK_EQ(r->data.size(), size_t{64});
           CHECK_EQ(r->readable.size(), size_t{64});
         }
       }},
      {"getmemex",
       [](XbdmClient &c) {
         auto r = c.getMemoryEx(0x82000000, 64);
         if (r) {
           CHECK_EQ(r->data.size(), size_t{64});
           CHECK_EQ(r->readable.size(), size_t{64});
         }
       }},
      {"setmem", [](XbdmClient &c) { (void)c.setMemory(0x82000000, ut::patternBytes(100, 5)); }},
      {"walkmem", [](XbdmClient &c) { (void)c.memoryRegions(); }},
      {"modules", [](XbdmClient &c) { (void)c.modules(); }},
      {"modsections", [](XbdmClient &c) { (void)c.moduleSections("m.xex"); }},
  };
}

} // namespace

TEST(XbdmFuzz, RandomAndMutatedAnswersToEveryCommand) {
  const auto ops = operations();
  const auto budget = std::chrono::seconds(4);
  const auto start = Clock::now();
  size_t iterations = 0;
  size_t succeeded = 0;
  Fuzzer fuzz(20261006);

  while (Clock::now() - start < budget || iterations < ops.size()) {
    const Op &op = ops[iterations % ops.size()];
    const Op &next = ops[fuzz.below(ops.size())];
    ++iterations;

    auto console = FakeConsole::create(false);
    console->send(fuzz.greeting());
    if (fuzz.chance(30)) console->setMaxRead(1 + fuzz.below(16));
    if (fuzz.chance(5)) console->hangUp();
    console->handle([&fuzz](FakeConsole &c, const std::string &line) {
      ut::Bytes reply = fuzz.answer();
      const bool binaryNext = line.rfind("sendfile ", 0) == 0 && reply.size() >= 4 &&
                              std::string(reply.begin(), reply.begin() + 4) == "204-";
      c.send(reply);
      if (binaryNext) {
        c.expectBinary(100, [&fuzz](FakeConsole &console, const ut::Bytes &) { console.send(fuzz.answer()); });
      }
      if (fuzz.chance(3)) c.hangUp();
    });

    ClientOptions options = xt::quickOptions();
    if (fuzz.chance(20)) options.maxLineBytes = 1 + fuzz.below(80);
    if (fuzz.chance(20)) options.maxBodyBytes = fuzz.below(200);
    if (fuzz.chance(10)) options.maxScreenshotBytes = fuzz.below(8);
    auto client = xt::attach(console, options);
    if (!client) {
      CHECK(console->closed());
      continue;
    }
    op.run(*client);
    CHECK(!client->transferActive());
    next.run(*client);
    if (client->isConnected()) ++succeeded;
  }
  CHECK(iterations >= ops.size());
  CHECK(succeeded > 0);
}
