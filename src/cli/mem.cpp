#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "cli/fileio.hpp"
#include "cli/session.hpp"

#include <format>
#include <memory>

namespace updclient::cli {

namespace {

struct MemArgs {
  uint64_t address = 0;
  uint64_t value = 0;
  uint32_t length = 16;
  std::string output = "1bl.bin";
};

Outcome<void> runPeek(Context &context, const MemArgs &args) {
  return withUpdServer(context, "", [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    const auto address = static_cast<uint32_t>(args.address);
    auto data = client.peek(address, args.length);
    if (!data) return fromError(data.error());
    const std::string hex = formatHex(*data);
    context.output.result({{"address", std::format("{:08X}", address)}, {"length", args.length}, {"data", hex}},
                          std::format("PEEK 0x{:08X} ({} bytes): {}", address, args.length, hex));
    return {};
  });
}

Outcome<void> runHvPeek(Context &context, const MemArgs &args) {
  return withUpdServer(context, "", [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto data = client.hvPeek(args.address, args.length);
    if (!data) return fromError(data.error());
    const std::string hex = formatHex(*data);
    context.output.result({{"hv_address", std::format("{:016X}", args.address)}, {"length", args.length}, {"data", hex}},
                          std::format("HVPEEK 0x{:016X} ({} bytes): {}", args.address, args.length, hex));
    return {};
  });
}

Outcome<void> runPoke(Context &context, const MemArgs &args) {
  const auto address = static_cast<uint32_t>(args.address);
  const auto value = static_cast<uint32_t>(args.value);
  const std::string action = std::format("WRITE 0x{:08X} to physical memory at 0x{:08X}", value, address);
  return withUpdServer(context, action, [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.poke(address, value);
    if (!result) return fromError(result.error());
    context.output.result({{"action", "poke"}, {"address", std::format("{:08X}", address)}, {"value", std::format("{:08X}", value)}, {"acknowledged", false}},
                          std::format("Sent POKE 0x{:08X} = 0x{:08X} (not acknowledged; verify with 'mem peek')", address, value));
    return {};
  });
}

Outcome<void> runHvPoke(Context &context, const MemArgs &args) {
  const std::string action =
      std::format("WRITE 0x{:016X} to hypervisor memory at 0x{:016X}", args.value, args.address);
  return withUpdServer(context, action, [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.hvPoke(args.address, args.value);
    if (!result) return fromError(result.error());
    context.output.result({{"action", "hvpoke"}, {"hv_address", std::format("{:016X}", args.address)}, {"value", std::format("{:016X}", args.value)}, {"acknowledged", false}},
                          std::format("Sent HVPOKE 0x{:016X} = 0x{:016X} (not acknowledged; verify with 'mem hvpeek')",
                                      args.address, args.value));
    return {};
  });
}

Outcome<void> runGet1bl(Context &context, const MemArgs &args) {
  return withUpdServer(context, "", [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto data = client.get1bl();
    if (!data) return fromError(data.error());
    if (auto written = writeFile(pathFromUtf8(args.output), *data); !written) return written;
    context.output.result({{"output", args.output}, {"bytes", data->size()}},
                          std::format("1BL written to {} ({} bytes)", args.output, data->size()));
    return {};
  });
}

} // namespace

void registerMemCommands(CLI::App &app, Context &context) {
  auto *mem = addGroup(app, "mem", "Memory peek/poke and hypervisor operations (UpdServer)");

  auto peekArgs = std::make_shared<MemArgs>();
  auto *peek = mem->add_subcommand("peek", "Peek physical memory");
  addNumber(peek, "address", peekArgs->address, "Physical address")->required();
  addNumber(peek, "length", peekArgs->length, "Length in bytes")->default_str("16");
  peek->callback([&context, peekArgs] {
    if (peekArgs->address > 0xFFFFFFFFull) {
      context.finish(usageError("address must fit in 32 bits (use hvpeek for 64-bit addresses)"));
      return;
    }
    context.finish(runPeek(context, *peekArgs));
  });

  auto pokeArgs = std::make_shared<MemArgs>();
  auto *poke = mem->add_subcommand("poke", "Poke a 32-bit value into physical memory (destructive: needs --yes or confirmation)");
  addNumber(poke, "address", pokeArgs->address, "Physical address")->required();
  addNumber(poke, "value", pokeArgs->value, "32-bit value")->required();
  poke->callback([&context, pokeArgs] {
    if (pokeArgs->address > 0xFFFFFFFFull || pokeArgs->value > 0xFFFFFFFFull) {
      context.finish(usageError("address and value must fit in 32 bits"));
      return;
    }
    context.finish(runPoke(context, *pokeArgs));
  });

  auto hvPeekArgs = std::make_shared<MemArgs>();
  auto *hvPeek = mem->add_subcommand("hvpeek", "Peek hypervisor (HV) memory space");
  addNumber(hvPeek, "address", hvPeekArgs->address, "HV address")->required();
  addNumber(hvPeek, "length", hvPeekArgs->length, "Length in bytes")->default_str("16");
  hvPeek->callback([&context, hvPeekArgs] { context.finish(runHvPeek(context, *hvPeekArgs)); });

  auto hvPokeArgs = std::make_shared<MemArgs>();
  auto *hvPoke = mem->add_subcommand("hvpoke", "Poke a 64-bit value into hypervisor memory (destructive: needs --yes or confirmation)");
  addNumber(hvPoke, "address", hvPokeArgs->address, "HV address")->required();
  addNumber(hvPoke, "value", hvPokeArgs->value, "64-bit value")->required();
  hvPoke->callback([&context, hvPokeArgs] { context.finish(runHvPoke(context, *hvPokeArgs)); });

  auto get1blArgs = std::make_shared<MemArgs>();
  auto *get1bl = mem->add_subcommand("get1bl", "Dump the 1BL ROM (32 KB) to a file");
  get1bl->add_option("-o,--output", get1blArgs->output, "Output file path")->capture_default_str();
  get1bl->callback([&context, get1blArgs] { context.finish(runGet1bl(context, *get1blArgs)); });
}

} // namespace updclient::cli
