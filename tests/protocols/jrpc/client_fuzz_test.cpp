#include "protocols/jrpc/client_fake.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/jrpc/client.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <variant>
#include <vector>

// Random and mutated console output for the banner and for every return kind. The
// client must neither crash nor hang, and must leave the connection in the state D3
// and D4 promise: usable after a good reply or an `error=` line, closed after anything
// else. Bounded by time, so it suits sanitizer runs.

using namespace updclient;
using namespace updclient::jrpc;
using jt::FakeConsole;

namespace {

using Clock = std::chrono::steady_clock;

struct Corpus {
  ReturnKind kind;
  size_t arraySize;
  std::string reply;
};

// Well-formed replies the mutations start from, one or more per return kind.
std::vector<Corpus> corpus() {
  return {
      {ReturnKind::Void, 0, "S_OK"},
      {ReturnKind::Void, 0, "0"},
      {ReturnKind::Void, 0, "82000000"},
      {ReturnKind::Int, 0, "2A"},
      {ReturnKind::Int, 0, "FFFFFFFF"},
      {ReturnKind::Byte, 0, "FF"},
      {ReturnKind::Byte, 0, "0"},
      {ReturnKind::Int64, 0, "248173A00"},
      {ReturnKind::Int64, 0, "FFFFFFFFFFFFFFFF"},
      {ReturnKind::Float, 0, "1.500000"},
      {ReturnKind::Float, 0, "-0.000000"},
      {ReturnKind::String, 0, "hello world"},
      {ReturnKind::String, 0, ""},
      {ReturnKind::IntArray, 3, "1,-2,3;"},
      {ReturnKind::IntArray, 8, "1,2,3,4,5,6,7,8;"},
      {ReturnKind::FloatArray, 2, "1.500000,2.000000;"},
      {ReturnKind::ByteArray, 3, "1,FF,A;"},
      {ReturnKind::ByteArray, 1, "0;"},
  };
}

bool valueMatchesKind(ReturnKind kind, const CallValue &value) {
  switch (kind) {
  case ReturnKind::Void: return std::holds_alternative<std::monostate>(value);
  case ReturnKind::Int:
  case ReturnKind::Byte:
  case ReturnKind::Int64: return std::holds_alternative<uint64_t>(value);
  case ReturnKind::Float: return std::holds_alternative<double>(value);
  case ReturnKind::String: return std::holds_alternative<std::string>(value);
  case ReturnKind::IntArray: return std::holds_alternative<std::vector<int32_t>>(value);
  case ReturnKind::FloatArray: return std::holds_alternative<std::vector<double>>(value);
  case ReturnKind::ByteArray: return std::holds_alternative<std::vector<uint8_t>>(value);
  case ReturnKind::Uint64Array: return false;
  }
  return false;
}

class Fuzzer {
public:
  explicit Fuzzer(uint32_t seed) : rng_(seed), corpus_(corpus()) {}

  size_t below(size_t n) { return n == 0 ? 0 : std::uniform_int_distribution<size_t>(0, n - 1)(rng_); }
  bool chance(int percent) { return static_cast<int>(below(100)) < percent; }
  uint8_t byte() { return static_cast<uint8_t>(below(256)); }
  const Corpus &pick() { return corpus_[below(corpus_.size())]; }

  // What the console sends for one command whose well-formed reply is `base`.
  std::string answer(const std::string &base) {
    if (chance(12)) return noise();
    if (chance(8)) return "error=" + text(below(60)) + "\r\n";
    if (chance(3)) return "DEBUG " + text(below(20)) + "\r\n";
    std::string out = base + (chance(8) ? "\n" : "\r\n");
    const size_t mutations = below(4);
    for (size_t m = 0; m < mutations; ++m) mutate(out);
    if (chance(8)) out += base + "\r\n";
    return out;
  }

  std::string banner() {
    std::string out = "JRPC2 connected\r\n";
    if (chance(60)) return out;
    if (chance(10)) return noise();
    const size_t mutations = 1 + below(3);
    for (size_t m = 0; m < mutations; ++m) mutate(out);
    return out;
  }

private:
  std::string noise() {
    static constexpr char kBias[] = "\r\n,;=. 0xA-DEBUGerror";
    std::string out(below(300), '\0');
    for (char &c : out) c = chance(15) ? kBias[below(sizeof(kBias) - 1)] : static_cast<char>(byte());
    return out;
  }

  std::string text(size_t length) {
    std::string out(length, 'a');
    for (char &c : out) c = static_cast<char>(' ' + below(95));
    return out;
  }

  void mutate(std::string &out) {
    if (out.empty()) return;
    const size_t at = below(out.size());
    switch (below(7)) {
    case 0: out[at] = static_cast<char>(byte()); break;
    case 1: out[at] = static_cast<char>(out[at] ^ (1u << below(8))); break;
    case 2: out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), static_cast<char>(byte())); break;
    case 3: out.erase(at, 1 + below(8)); break;
    case 4: out.resize(at); break;
    case 5: out.insert(at, below(2) ? "\r" : "\n"); break;
    case 6: out.insert(at, std::string(1 + below(6), "0123456789abcdefFqx,;"[below(21)])); break;
    }
  }

  std::mt19937 rng_;
  std::vector<Corpus> corpus_;
};

// The first line of what was sent, the way the client reads it: up to the first LF, one
// trailing CR removed. nullopt when no LF came.
std::optional<std::string> firstLine(const std::string &bytes) {
  const size_t lf = bytes.find('\n');
  if (lf == std::string::npos) return std::nullopt;
  std::string line = bytes.substr(0, lf);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return line;
}

bool isClosingFailure(ErrorCode code) {
  return code == ErrorCode::Protocol || code == ErrorCode::Timeout || code == ErrorCode::Disconnected ||
         code == ErrorCode::LimitExceeded || code == ErrorCode::Unsupported;
}

} // namespace

TEST(JrpcClientFuzz, RandomAndMutatedBanners) {
  Fuzzer fuzz(20261008);
  size_t accepted = 0;
  for (int round = 0; round < 4000; ++round) {
    const std::string bytes = fuzz.banner();
    auto console = FakeConsole::create(false);
    console->send(bytes);
    if (fuzz.chance(30)) console->setMaxRead(1 + fuzz.below(8));
    if (fuzz.chance(10)) console->hangUp();
    auto client = jt::attach(console);
    const auto line = firstLine(bytes);
    const bool expected = line && *line == "JRPC2 connected";
    if (expected) {
      CHECK(client.has_value());
      if (client) {
        ++accepted;
        CHECK(client->isConnected());
      }
    } else {
      CHECK(!client.has_value());
      if (!client) {
        CHECK_MSG(isClosingFailure(client.error().code), formatError(client.error()));
        CHECK(console->closed());
      }
    }
  }
  CHECK(accepted > 0);
}

TEST(JrpcClientFuzz, RandomAndMutatedRepliesToEveryReturnKind) {
  const auto budget = std::chrono::seconds(3);
  const auto start = Clock::now();
  Fuzzer fuzz(20261009);
  size_t iterations = 0;
  size_t succeeded = 0;
  size_t remoteErrors = 0;

  while (Clock::now() - start < budget || iterations < 2000) {
    ++iterations;
    const Corpus &entry = fuzz.pick();
    const CallSpec spec = jt::callAt(0x82000000, entry.kind, {}, entry.arraySize);

    auto console = FakeConsole::create();
    if (fuzz.chance(30)) console->setMaxRead(1 + fuzz.below(16));
    std::string sent;
    console->handle([&](FakeConsole &c, const std::string &) {
      std::string bytes = fuzz.answer(entry.reply);
      sent += bytes;
      c.send(bytes);
      if (fuzz.chance(3)) c.hangUp();
    });
    auto client = jt::attach(console);
    REQUIRE_OK(client);
    // A small limit applies to the replies; the banner has been read already.
    if (fuzz.chance(20)) {
      ClientOptions options = client->options();
      options.maxReplyBytes = 1 + fuzz.below(40);
      client->setOptions(options);
    }

    for (int call = 0; call < 2; ++call) {
      const bool wasConnected = client->isConnected();
      auto r = client->call(spec);
      if (r) {
        ++succeeded;
        CHECK(valueMatchesKind(entry.kind, r->value));
        CHECK(client->isConnected());
        CHECK(!isErrorLine(r->line));
        CHECK(sent.find(r->line) != std::string::npos);
        // A reply that would not fit the limit cannot have been accepted.
        CHECK(r->line.size() <= client->options().maxReplyBytes);
      } else if (remoteFault(r.error())) {
        ++remoteErrors;
        CHECK(client->isConnected());
        CHECK_EQ(r.error().code, ErrorCode::Io);
        CHECK(sent.find("error=") != std::string::npos);
      } else {
        CHECK_MSG(isClosingFailure(r.error().code) || (!wasConnected && r.error().code == ErrorCode::NotConnected),
                  formatError(r.error()));
        CHECK(!client->isConnected());
        CHECK(console->closed());
      }
      const auto delivery = client->lastDelivery();
      CHECK(delivery.has_value());
    }
    // However it ended, a closed client stays closed and sends nothing more.
    if (!client->isConnected()) {
      const size_t written = console->written();
      CHECK_ERR(client->call(spec), ErrorCode::NotConnected);
      CHECK_EQ(console->written(), written);
    }
  }
  CHECK(succeeded > 0);
  CHECK(remoteErrors > 0);
}

TEST(JrpcClientFuzz, RandomRawCommandsAndAnswers) {
  Fuzzer fuzz(20261010);
  for (int round = 0; round < 1500; ++round) {
    auto console = FakeConsole::create();
    console->handle([&](FakeConsole &c, const std::string &) { c.send(fuzz.answer("2A")); });
    auto client = jt::attach(console);
    REQUIRE_OK(client);
    auto r = client->rawCommand("anything goes");
    if (r) {
      CHECK(client->isConnected());
      CHECK_EQ(r->isError, r->line.rfind("error=", 0) == 0);
    } else {
      CHECK_MSG(isClosingFailure(r.error().code), formatError(r.error()));
      CHECK(!client->isConnected());
    }
  }
}
