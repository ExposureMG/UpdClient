#include "cli/jrpc.hpp"

#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "cli/output.hpp"
#include "cli/session.hpp"

#include <core/hex.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace updclient::cli {

namespace {

std::string lowered(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::vector<std::string> splitOn(const std::string &text, char separator) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    const size_t at = text.find(separator, start);
    parts.push_back(text.substr(start, at == std::string::npos ? std::string::npos : at - start));
    if (at == std::string::npos) return parts;
    start = at + 1;
  }
}

// A signed decimal or 0x-prefixed hex number in [min, max]; parseUnsigned has no sign.
Result<int64_t> parseSigned(std::string_view text, int64_t min, int64_t max) {
  const bool negative = !text.empty() && text.front() == '-';
  const std::string_view digits = negative ? text.substr(1) : text;
  // The largest magnitude the sign allows; -min does not fit int64_t for the lowest value.
  const uint64_t limit = negative ? uint64_t{0} - static_cast<uint64_t>(min) : static_cast<uint64_t>(max);
  auto magnitude = parseUnsigned(digits, limit);
  if (!magnitude) {
    return fail(ErrorCode::InvalidArgument, "'" + std::string(text) + "' is not a number from " +
                                                std::to_string(min) + " to " + std::to_string(max));
  }
  return negative ? static_cast<int64_t>(uint64_t{0} - *magnitude) : static_cast<int64_t>(*magnitude);
}

template <class T> Result<T> parseReal(std::string_view text) {
  T value{};
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size()) {
    return fail(ErrorCode::InvalidArgument, "'" + std::string(text) + "' is not a decimal number");
  }
  return value;
}

std::string hexU32(uint32_t value) { return std::format("{:08X}", value); }

// ---- jrpc call ----------------------------------------------------------------------

struct CallArgs {
  std::string target;
  bool system = false;
  std::string returns = "void";
  std::optional<uint32_t> count;
  std::vector<std::string> args;
};

struct KindName {
  const char *name;
  jrpc::ReturnKind kind;
};

constexpr std::array<KindName, 9> kKinds = {{{"void", jrpc::ReturnKind::Void},
                                             {"int", jrpc::ReturnKind::Int},
                                             {"str", jrpc::ReturnKind::String},
                                             {"float", jrpc::ReturnKind::Float},
                                             {"byte", jrpc::ReturnKind::Byte},
                                             {"int64", jrpc::ReturnKind::Int64},
                                             {"ints", jrpc::ReturnKind::IntArray},
                                             {"floats", jrpc::ReturnKind::FloatArray},
                                             {"bytes", jrpc::ReturnKind::ByteArray}}};

// "0x82000000" (an address) or "xam.xex!656" (an export ordinal of a loaded module).
Outcome<std::variant<uint32_t, jrpc::ByName>> parseTarget(const std::string &text) {
  const size_t bang = text.rfind('!');
  if (bang == std::string::npos) {
    auto address = parseUnsigned(text, 0xFFFFFFFFull);
    if (!address) {
      return usageError("call target '" + text + "' is neither an address (0x82000000) nor module!ordinal (xam.xex!656)");
    }
    return std::variant<uint32_t, jrpc::ByName>(static_cast<uint32_t>(*address));
  }
  auto ordinal = parseUnsigned(text.substr(bang + 1), 0xFFFFFFFFull);
  if (!ordinal || bang == 0) {
    return usageError("call target '" + text + "' is not module!ordinal, e.g. xam.xex!656");
  }
  return std::variant<uint32_t, jrpc::ByName>(jrpc::ByName{text.substr(0, bang), static_cast<uint32_t>(*ordinal)});
}

// type:value, e.g. i32:-5 u32:0xFFFFFFFF bool:true byte:255 i64:-1 u64:0x100000000
// f32:1.5 f64:2.25 str:hello bytes:DEADBEEF ints:1,2,3 floats:0.5,1.5
Outcome<jrpc::Arg> parseArg(const std::string &text) {
  const size_t colon = text.find(':');
  if (colon == std::string::npos) {
    return usageError("--arg '" + text + "' must look like type:value, e.g. i32:5 or str:hello");
  }
  const std::string type = lowered(text.substr(0, colon));
  const std::string value = text.substr(colon + 1);
  const auto bad = [&](const std::string &why) { return usageError("--arg '" + text + "': " + why); };

  if (type == "i32") {
    auto v = parseSigned(value, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
    if (!v) return bad(v.error().message);
    return jrpc::Arg::i32(static_cast<int32_t>(*v));
  }
  if (type == "u32") {
    auto v = parseUnsigned(value, 0xFFFFFFFFull);
    if (!v) return bad(v.error().message);
    return jrpc::Arg::u32(static_cast<uint32_t>(*v));
  }
  if (type == "bool") {
    const std::string word = lowered(value);
    if (word == "true" || word == "1") return jrpc::Arg::boolean(true);
    if (word == "false" || word == "0") return jrpc::Arg::boolean(false);
    return bad("a bool is true, false, 1 or 0");
  }
  if (type == "byte") {
    auto v = parseUnsigned(value, 0xFF);
    if (!v) return bad(v.error().message);
    return jrpc::Arg::byte(static_cast<uint8_t>(*v));
  }
  if (type == "i64") {
    auto v = parseSigned(value, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
    if (!v) return bad(v.error().message);
    return jrpc::Arg::i64(*v);
  }
  if (type == "u64") {
    auto v = parseUnsigned(value);
    if (!v) return bad(v.error().message);
    return jrpc::Arg::u64(*v);
  }
  if (type == "f32" || type == "f64") {
    if (type == "f32") {
      auto v = parseReal<float>(value);
      if (!v) return bad(v.error().message);
      return jrpc::Arg::f32(*v);
    }
    auto v = parseReal<double>(value);
    if (!v) return bad(v.error().message);
    return jrpc::Arg::f64(*v);
  }
  if (type == "str") return jrpc::Arg::string(value);
  if (type == "bytes") {
    std::string_view digits = value;
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits.remove_prefix(2);
    if (digits.size() % 2 != 0) return bad("a byte blob needs an even number of hex digits");
    std::vector<uint8_t> bytes(digits.size() / 2);
    if (auto parsed = parseHex(digits, bytes); !parsed) return bad(parsed.error().message);
    return jrpc::Arg::bytes(std::move(bytes));
  }
  if (type == "ints") {
    std::vector<int32_t> ints;
    if (!value.empty()) {
      for (const auto &part : splitOn(value, ',')) {
        auto v = parseSigned(part, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
        if (!v) return bad(v.error().message);
        ints.push_back(static_cast<int32_t>(*v));
      }
    }
    return jrpc::Arg::ints(std::move(ints));
  }
  if (type == "floats") {
    std::vector<float> floats;
    if (!value.empty()) {
      for (const auto &part : splitOn(value, ',')) {
        auto v = parseReal<float>(part);
        if (!v) return bad(v.error().message);
        floats.push_back(*v);
      }
    }
    return jrpc::Arg::floats(std::move(floats));
  }
  return bad("unknown type '" + type + "' (i32, u32, bool, byte, i64, u64, f32, f64, str, bytes, ints, floats)");
}

std::string joined(const auto &values) {
  std::string out;
  for (const auto &v : values) out += (out.empty() ? "" : ", ") + std::format("{}", v);
  return out;
}

// The decoded value of a call, for the "value" member of the JSON result.
nlohmann::json valueJson(const jrpc::CallValue &value) {
  if (const auto *number = std::get_if<uint64_t>(&value)) return *number;
  if (const auto *text = std::get_if<std::string>(&value)) return *text;
  if (const auto *real = std::get_if<double>(&value)) return *real;
  if (const auto *ints = std::get_if<std::vector<int32_t>>(&value)) return *ints;
  if (const auto *reals = std::get_if<std::vector<double>>(&value)) return *reals;
  if (const auto *bytes = std::get_if<std::vector<uint8_t>>(&value)) return formatHex(*bytes);
  return nullptr;
}

std::string valueText(jrpc::ReturnKind kind, const jrpc::CallValue &value) {
  if (const auto *number = std::get_if<uint64_t>(&value)) {
    switch (kind) {
    case jrpc::ReturnKind::Int: {
      const auto word = static_cast<uint32_t>(*number);
      return word > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
                 ? std::format("{} (0x{:08X}, signed {})", word, word, static_cast<int32_t>(word))
                 : std::format("{} (0x{:08X})", word, word);
    }
    case jrpc::ReturnKind::Byte: return std::format("{} (0x{:02X})", *number, *number);
    default:
      return *number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                 ? std::format("{} (0x{:016X}, signed {})", *number, *number, static_cast<int64_t>(*number))
                 : std::format("{} (0x{:016X})", *number, *number);
    }
  }
  if (const auto *text = std::get_if<std::string>(&value)) return terminalText(*text);
  if (const auto *real = std::get_if<double>(&value)) return std::format("{}", *real);
  if (const auto *ints = std::get_if<std::vector<int32_t>>(&value)) return joined(*ints);
  if (const auto *reals = std::get_if<std::vector<double>>(&value)) return joined(*reals);
  if (const auto *bytes = std::get_if<std::vector<uint8_t>>(&value)) return formatHex(*bytes);
  return "(none)";
}

Outcome<void> runCall(Context &context, const CallArgs &args) {
  // Everything that can be wrong with the request is found before a connection is
  // made or a confirmation asked for.
  auto target = parseTarget(args.target);
  if (!target) return unexpected<Failure>(target.error());

  const auto kindIt = std::find_if(kKinds.begin(), kKinds.end(),
                                   [&](const KindName &k) { return lowered(args.returns) == k.name; });
  if (kindIt == kKinds.end()) {
    return usageError("--returns '" + args.returns + "' is not one of void, int, str, float, byte, int64, ints, floats, bytes");
  }

  jrpc::CallSpec spec;
  spec.target = *target;
  spec.thread = args.system ? jrpc::ThreadContext::System : jrpc::ThreadContext::Title;
  spec.returns = kindIt->kind;
  if (jrpc::isArrayKind(spec.returns)) {
    if (!args.count) {
      return usageError("--returns " + std::string(kindIt->name) + " needs --count, the number of elements to read (1 to " +
                        std::to_string(jrpc::kMaxArrayElements) + ")");
    }
    if (*args.count < 1 || *args.count > jrpc::kMaxArrayElements) {
      return usageError("--count must be 1 to " + std::to_string(jrpc::kMaxArrayElements) + ", the most the server returns");
    }
    spec.arraySize = *args.count;
  } else if (args.count) {
    return usageError("--count only applies to --returns ints, floats or bytes");
  }
  for (const auto &text : args.args) {
    auto arg = parseArg(text);
    if (!arg) return unexpected<Failure>(arg.error());
    spec.args.push_back(std::move(*arg));
  }
  if (auto built = jrpc::buildCommand(spec); !built) return fromError(built.error());

  const std::string action = std::format("call {}{} on the console, which runs code there", args.target,
                                         args.system ? " in the system thread context" : "");
  return withJrpc(context, JrpcEffect::ChangesConsole, action, [&](jrpc::JrpcClient &client, const net::Endpoint &endpoint) -> Outcome<void> {
    auto result = client.call(spec);
    if (!result) return fromError(result.error());
    context.output.result({{"target", endpoint.toString()},
                           {"type", kindIt->name},
                           {"value", valueJson(result->value)},
                           {"line", result->line}},
                          std::format("Called {} ({})\n  reply : {}\n  value : {}", args.target, kindIt->name,
                                      terminalText(result->line), valueText(spec.returns, result->value)));
    return {};
  });
}

// ---- the other commands -----------------------------------------------------------

struct Sensor {
  const char *key;
  const char *label;
  jrpc::TemperatureSensor sensor;
};

constexpr std::array<Sensor, 4> kSensors = {{{"cpu", "CPU", jrpc::TemperatureSensor::Cpu},
                                             {"gpu", "GPU", jrpc::TemperatureSensor::Gpu},
                                             {"edram", "EDRAM", jrpc::TemperatureSensor::Edram},
                                             {"board", "Mainboard", jrpc::TemperatureSensor::Mainboard}}};

// Asks a console one question after another for the info command. A question the
// console does not answer leaves its field empty; a lost connection ends the asking.
// Ctrl-C is not a lost field: it is remembered so the command can end as Cancelled.
class Gatherer {
public:
  explicit Gatherer(jrpc::JrpcClient &client) : client_(client) {}

  template <class F> auto ask(std::string_view what, F &&call) -> std::optional<typename std::invoke_result_t<F>::value_type> {
    // A connection lost after a failed question is already explained by that failure.
    // One lost with no failure behind it may be a Ctrl-C between two questions: ask
    // once more, so the client says Cancelled (nothing is sent on a closed connection).
    if (!client_.isConnected() && first_) return std::nullopt;
    auto result = call();
    if (result) {
      ++answered_;
      return std::move(*result);
    }
    if (result.error().code == ErrorCode::Cancelled) {
      if (!cancelled_) cancelled_ = result.error();
    } else {
      spdlog::warn("{}: {}", what, result.error().message);
    }
    if (!first_) first_ = result.error();
    return std::nullopt;
  }

  size_t answered() const noexcept { return answered_; }
  const std::optional<Error> &firstError() const noexcept { return first_; }
  const std::optional<Error> &cancelled() const noexcept { return cancelled_; }

private:
  jrpc::JrpcClient &client_;
  size_t answered_ = 0;
  std::optional<Error> first_;
  std::optional<Error> cancelled_;
};

nlohmann::json optionalJson(const std::optional<uint32_t> &value) {
  return value ? nlohmann::json(*value) : nlohmann::json();
}

std::string temperatureText(const std::optional<uint32_t> &raw) {
  return raw ? std::format("{} (0x{:X})", *raw, *raw) : std::string("(not answered)");
}

Outcome<void> runPing(Context &context) {
  auto found = identifyJrpc(context);
  if (!found) return unexpected<Failure>(found.error());
  context.output.result({{"target", found->endpoint.toString()}, {"banner", found->banner}},
                        std::format("JRPC is installed at {} (it greeted with \"{}\")", found->endpoint.toString(),
                                    terminalText(found->banner)));
  return {};
}

Outcome<void> runResolve(Context &context, const std::string &module, uint32_t ordinal) {
  if (module.empty()) return usageError("the module name is empty");
  return withJrpc(context, JrpcEffect::ReadOnly, "", [&](jrpc::JrpcClient &client, const net::Endpoint &) -> Outcome<void> {
    auto address = client.resolveFunction(module, ordinal);
    if (!address) return fromError(address.error());
    context.output.result({{"module", module}, {"ordinal", ordinal}, {"address", hexU32(*address)}},
                          std::format("{}!{} = 0x{:08X}", module, ordinal, *address));
    return {};
  });
}

Outcome<void> runCpuKey(Context &context) {
  return withJrpc(context, JrpcEffect::ReadOnly, "", [&](jrpc::JrpcClient &client, const net::Endpoint &) -> Outcome<void> {
    auto key = client.cpuKey();
    if (!key) return fromError(key.error());
    context.output.result({{"cpu_key", key->hex()}}, "CPU key: " + key->hex());
    return {};
  });
}

Outcome<void> runKernel(Context &context) {
  return withJrpc(context, JrpcEffect::ReadOnly, "", [&](jrpc::JrpcClient &client, const net::Endpoint &) -> Outcome<void> {
    auto version = client.kernelVersion();
    if (!version) return fromError(version.error());
    context.output.result({{"kernel_version", *version}}, std::format("Kernel version: {}", *version));
    return {};
  });
}

Outcome<void> runConsoleType(Context &context) {
  return withJrpc(context, JrpcEffect::ReadOnly, "", [&](jrpc::JrpcClient &client, const net::Endpoint &) -> Outcome<void> {
    auto type = client.consoleType();
    if (!type) return fromError(type.error());
    const std::string name(jrpc::consoleTypeName(*type));
    context.output.result({{"console_type", name}}, "Console type: " + name);
    return {};
  });
}

Outcome<void> runTitleId(Context &context) {
  return withJrpc(context, JrpcEffect::ReadOnly, "", [&](jrpc::JrpcClient &client, const net::Endpoint &) -> Outcome<void> {
    auto title = client.currentTitleId();
    if (!title) return fromError(title.error());
    context.output.result({{"title_id", hexU32(*title)}}, "Running title: " + hexU32(*title));
    return {};
  });
}

Outcome<void> runTemp(Context &context, const std::string &which) {
  std::vector<Sensor> chosen;
  const std::string word = lowered(which);
  for (const auto &sensor : kSensors) {
    if (word == "all" || word == sensor.key || (word == "mainboard" && sensor.sensor == jrpc::TemperatureSensor::Mainboard)) {
      chosen.push_back(sensor);
    }
  }
  if (chosen.empty()) return usageError("'" + which + "' is not a sensor (cpu, gpu, edram, board or all)");
  return withJrpc(context, JrpcEffect::ReadOnly, "", [&](jrpc::JrpcClient &client, const net::Endpoint &) -> Outcome<void> {
    nlohmann::json values = nlohmann::json::object();
    std::string text;
    for (const auto &sensor : chosen) {
      auto raw = client.temperature(sensor.sensor);
      if (!raw) return fromError(raw.error());
      values[sensor.key] = *raw;
      text += std::format("{:<10} {} (raw value)\n", std::string(sensor.label) + ":", temperatureText(*raw));
    }
    context.output.result({{"temperatures", values}}, text);
    return {};
  });
}

Outcome<void> runNotify(Context &context, const std::string &message, uint32_t type) {
  if (message.empty()) return usageError("the notification text is empty");
  if (auto encoded = jrpc::encodeArgument(jrpc::Arg::string(message)); !encoded) return fromError(encoded.error());
  return withJrpc(context, JrpcEffect::ChangesConsole, "", [&](jrpc::JrpcClient &client, const net::Endpoint &endpoint) -> Outcome<void> {
    auto r = client.notify(message, type);
    if (!r) return fromError(r.error());
    context.output.result({{"action", "notify"}, {"target", endpoint.toString()}, {"text", message}, {"type", type}},
                          std::format("Sent a notification (type {}) to {}", type, endpoint.toString()));
    return {};
  });
}

Outcome<jrpc::LedState> parseLed(const std::string &text) {
  const std::string word = lowered(text);
  if (word == "off") return jrpc::LedState::Off;
  if (word == "red") return jrpc::LedState::Red;
  if (word == "green") return jrpc::LedState::Green;
  if (word == "orange") return jrpc::LedState::Orange;
  auto raw = parseUnsigned(text, 0xFFFFFFFFull);
  if (!raw) return usageError("LED '" + text + "' is not off, red, green, orange or a raw number");
  return static_cast<jrpc::LedState>(static_cast<uint32_t>(*raw));
}

Outcome<void> runLeds(Context &context, const std::array<std::string, 4> &names) {
  std::array<jrpc::LedState, 4> leds{};
  for (size_t i = 0; i < names.size(); ++i) {
    auto led = parseLed(names[i]);
    if (!led) return unexpected<Failure>(led.error());
    leds[i] = *led;
  }
  const auto raw = [&](size_t i) { return static_cast<uint32_t>(leds[i]); };
  const std::string action = std::format("set the four console LEDs to raw values 0x{:02X} 0x{:02X} 0x{:02X} 0x{:02X}",
                                         raw(0), raw(1), raw(2), raw(3));
  return withJrpc(context, JrpcEffect::ChangesConsole, action, [&](jrpc::JrpcClient &client, const net::Endpoint &endpoint) -> Outcome<void> {
    auto r = client.setLeds(leds[0], leds[1], leds[2], leds[3]);
    if (!r) return fromError(r.error());
    context.output.result({{"action", "leds"},
                           {"target", endpoint.toString()},
                           {"leds", nlohmann::json::array({raw(0), raw(1), raw(2), raw(3)})}},
                          std::format("Set the LEDs of {} to 0x{:02X} 0x{:02X} 0x{:02X} 0x{:02X}", endpoint.toString(),
                                      raw(0), raw(1), raw(2), raw(3)));
    return {};
  });
}

struct ConstMemArgs {
  uint32_t address = 0;
  uint32_t value = 0;
  std::optional<uint32_t> ifValue;
  std::optional<uint32_t> inTitle;
};

Outcome<void> runConstMem(Context &context, const ConstMemArgs &args) {
  if (args.address == 0) return usageError("the address must not be 0");
  std::string action = std::format("register a task that keeps writing 0x{:08X} at 0x{:08X}", args.value, args.address);
  if (args.ifValue) action += std::format(" while the word there is 0x{:08X}", *args.ifValue);
  if (args.inTitle) action += std::format(" while title {:08X} runs", *args.inTitle);
  action += " (this client cannot remove the task)";
  return withJrpc(context, JrpcEffect::ChangesConsole, action, [&](jrpc::JrpcClient &client, const net::Endpoint &endpoint) -> Outcome<void> {
    spdlog::warn("A constant-memory task cannot be removed by this client");
    auto r = client.constantMemorySet(args.address, args.value, args.ifValue, args.inTitle);
    if (!r) return fromError(r.error());
    context.output.result({{"action", "constmem"},
                           {"target", endpoint.toString()},
                           {"address", hexU32(args.address)},
                           {"value", hexU32(args.value)},
                           {"if_value", args.ifValue ? nlohmann::json(hexU32(*args.ifValue)) : nlohmann::json()},
                           {"in_title", args.inTitle ? nlohmann::json(hexU32(*args.inTitle)) : nlohmann::json()}},
                          std::format("Registered a task that keeps writing 0x{:08X} at 0x{:08X} on {}", args.value,
                                      args.address, endpoint.toString()));
    return {};
  });
}

Outcome<void> runRaw(Context &context, const std::string &line) {
  if (line.empty()) return usageError("the command line is empty");
  if (!std::all_of(line.begin(), line.end(), [](unsigned char c) { return c >= 0x20 && c <= 0x7E; })) {
    return usageError("a raw command line is printable ASCII only");
  }
  return withJrpc(context, JrpcEffect::ChangesConsole, "send the raw command '" + line + "'",
                  [&](jrpc::JrpcClient &client, const net::Endpoint &) -> Outcome<void> {
                    auto answer = client.rawCommand(line);
                    if (!answer) return fromError(answer.error());
                    context.output.result({{"command", line}, {"reply", answer->line}, {"is_error", answer->isError}},
                                          terminalText(answer->line));
                    return {};
                  });
}

} // namespace

Outcome<void> jrpcInfo(Context &context) {
  return withJrpc(context, JrpcEffect::ReadOnly, "", [&](jrpc::JrpcClient &client, const net::Endpoint &endpoint) -> Outcome<void> {
    Gatherer gather(client);
    const auto kernel = gather.ask("kernel version", [&] { return client.kernelVersion(); });
    const auto type = gather.ask("console type", [&] { return client.consoleType(); });
    const auto title = gather.ask("running title", [&] { return client.currentTitleId(); });
    const auto key = gather.ask("CPU key", [&] { return client.cpuKey(); });
    std::array<std::optional<uint32_t>, kSensors.size()> temperatures;
    for (size_t i = 0; i < kSensors.size(); ++i) {
      temperatures[i] = gather.ask(std::string(kSensors[i].label) + " temperature",
                                   [&] { return client.temperature(kSensors[i].sensor); });
    }
    // Ctrl-C ends the command, however many fields were read before it.
    if (gather.cancelled()) return fromError(*gather.cancelled());
    // Nothing answered at all: that is a failure, not an empty report.
    if (gather.answered() == 0 && gather.firstError()) return fromError(*gather.firstError());
    if (!client.isConnected()) spdlog::warn("The connection was lost; the fields after that point were not asked");

    nlohmann::json temps = nlohmann::json::object();
    for (size_t i = 0; i < kSensors.size(); ++i) temps[kSensors[i].key] = optionalJson(temperatures[i]);
    nlohmann::json json = {{"target", endpoint.toString()},
                           {"kernel_version", kernel ? nlohmann::json(*kernel) : nlohmann::json()},
                           {"console_type", type ? nlohmann::json(std::string(jrpc::consoleTypeName(*type))) : nlohmann::json()},
                           {"title_id", title ? nlohmann::json(hexU32(*title)) : nlohmann::json()},
                           {"cpu_key", key ? nlohmann::json(key->hex()) : nlohmann::json()},
                           {"temperatures", temps}};
    const auto shown = [](const std::optional<std::string> &value) { return value ? *value : std::string("(not answered)"); };
    std::string text;
    text += "================ JRPC CONSOLE INFO ================\n";
    text += std::format("  Kernel Version  : {}\n", shown(kernel ? std::optional<std::string>(std::to_string(*kernel)) : std::nullopt));
    text += std::format("  Console Type    : {}\n", shown(type ? std::optional<std::string>(std::string(jrpc::consoleTypeName(*type))) : std::nullopt));
    text += std::format("  Running Title   : {}\n", shown(title ? std::optional<std::string>(hexU32(*title)) : std::nullopt));
    text += std::format("  CPU Key         : {}\n", shown(key ? std::optional<std::string>(key->hex()) : std::nullopt));
    for (size_t i = 0; i < kSensors.size(); ++i) {
      text += std::format("  {:<16}: {}\n", std::string(kSensors[i].label) + " Temp", temperatureText(temperatures[i]));
    }
    text += "===================================================\n";
    context.output.result(json, text);
    return {};
  });
}

Outcome<void> jrpcShutdown(Context &context) {
  return withJrpc(context, JrpcEffect::ChangesConsole, "shut the console down",
                  [&](jrpc::JrpcClient &client, const net::Endpoint &endpoint) -> Outcome<void> {
                    auto r = client.shutdown();
                    if (!r) return fromError(r.error());
                    context.output.result({{"action", "shutdown"}, {"target", endpoint.toString()}, {"acknowledged", false}},
                                          "Sent shutdown to " + endpoint.toString() +
                                              " (the console does not answer it; the connection was closed)");
                    return {};
                  });
}

void registerJrpcCommands(CLI::App &app, Context &context) {
  auto *group = addGroup(app, "jrpc",
                         "JRPC plugin operations on port 1409 (--target jrpc://host or a bare host; JRPC is never "
                         "auto-discovered). Read-only commands run at once; call, raw, shutdown, leds and constmem "
                         "are destructive (need --yes or confirmation)");

  auto text = std::make_shared<std::string>();
  auto number = std::make_shared<uint32_t>(0);

  auto *ping = group->add_subcommand("ping", "Check that JRPC is installed: connect, read the banner, say Bye");
  ping->callback([&context] { context.finish(runPing(context)); });

  auto *info = group->add_subcommand("info", "Kernel version, console type, running title, CPU key and temperatures");
  info->callback([&context] { context.finish(jrpcInfo(context)); });

  auto *resolve = group->add_subcommand("resolve", "Resolve an export of a loaded module to its address");
  resolve->add_option("module", *text, "Module name, e.g. xam.xex")->required();
  addNumber(resolve, "ordinal", *number, "Export ordinal")->required();
  resolve->callback([&context, text, number] { context.finish(runResolve(context, *text, *number)); });

  auto *cpukey = group->add_subcommand("cpukey", "Read the CPU key");
  cpukey->callback([&context] { context.finish(runCpuKey(context)); });

  auto *kernel = group->add_subcommand("kernel", "Read the kernel version");
  kernel->callback([&context] { context.finish(runKernel(context)); });

  auto *consoleType = group->add_subcommand("console-type", "Read the motherboard family (Xenon, Zephyr, Falcon, Jasper, Trinity, Corona)");
  consoleType->callback([&context] { context.finish(runConsoleType(context)); });

  auto *titleId = group->add_subcommand("title-id", "Read the title id of the running title");
  titleId->callback([&context] { context.finish(runTitleId(context)); });

  auto sensor = std::make_shared<std::string>("all");
  auto *temp = group->add_subcommand("temp", "Read a temperature sensor as the console reports it (raw value, unit unknown)");
  temp->add_option("sensor", *sensor, "cpu, gpu, edram, board or all")->capture_default_str();
  temp->callback([&context, sensor] { context.finish(runTemp(context, *sensor)); });

  auto notifyType = std::make_shared<uint32_t>(0);
  auto *notify = group->add_subcommand("notify", "Show a notification on the console");
  notify->add_option("text", *text, "The notification text")->required();
  addNumber(notify, "--type", *notifyType, "The icon type the console shows")->default_str("0");
  notify->callback([&context, text, notifyType] { context.finish(runNotify(context, *text, *notifyType)); });

  auto leds = std::make_shared<std::array<std::string, 4>>();
  auto *ledsCommand = group->add_subcommand("leds", "Set the four ring LEDs (destructive: needs --yes or confirmation)");
  const char *ledNames[4] = {"top-left", "top-right", "bottom-left", "bottom-right"};
  for (size_t i = 0; i < 4; ++i) {
    ledsCommand->add_option(ledNames[i], (*leds)[i], "off, red, green, orange or a raw number")->required();
  }
  ledsCommand->callback([&context, leds] { context.finish(runLeds(context, *leds)); });

  auto *shutdown = group->add_subcommand("shutdown", "Shut the console down (destructive: needs --yes or confirmation)");
  shutdown->callback([&context] { context.finish(jrpcShutdown(context)); });

  auto constMem = std::make_shared<ConstMemArgs>();
  auto *constmem = group->add_subcommand("constmem", "Register a task that keeps writing a word to memory; this client cannot remove it "
                                                     "(destructive: needs --yes or confirmation)");
  addNumber(constmem, "address", constMem->address, "Address to write")->required();
  addNumber(constmem, "value", constMem->value, "32-bit value to write")->required();
  addNumber(constmem, "--if-value", constMem->ifValue, "Write only while the word at the address holds this value");
  addNumber(constmem, "--in-title", constMem->inTitle, "Write only while this title id is running");
  constmem->callback([&context, constMem] { context.finish(runConstMem(context, *constMem)); });

  auto callArgs = std::make_shared<CallArgs>();
  auto *call = group->add_subcommand("call", "Call a function on the console (destructive: needs --yes or confirmation)");
  call->add_option("target", callArgs->target, "Address (0x82000000) or module!ordinal (xam.xex!656)")->required();
  call->add_flag("--system", callArgs->system, "Run in the system thread context instead of the title's");
  call->add_option("--returns", callArgs->returns, "What the function returns: void, int, str, float, byte, int64, ints, floats or bytes")
      ->capture_default_str();
  addNumber(call, "--count", callArgs->count, "Elements to read for --returns ints, floats or bytes (1 to 8)");
  call->add_option("--arg", callArgs->args,
                   "An argument as type:value, repeatable: i32, u32, bool, byte, i64, u64, f32, f64, str, bytes (hex), "
                   "ints and floats (comma separated), e.g. --arg i32:5 --arg str:hi --arg bytes:DEADBEEF")
      ->expected(1)
      ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
  call->callback([&context, callArgs] { context.finish(runCall(context, *callArgs)); });

  auto *raw = group->add_subcommand("raw", "Send one command line as typed and print the reply line, for diagnostics "
                                           "(destructive: needs --yes or confirmation)");
  raw->add_option("line", *text, "The command line, e.g. 'consolefeatures ver=2 type=13 params=\"A\\0\\A\\0\\\"'")->required();
  raw->callback([&context, text] { context.finish(runRaw(context, *text)); });
}

} // namespace updclient::cli
