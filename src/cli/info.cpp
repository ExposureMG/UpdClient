#include "cli/commands.hpp"
#include "cli/session.hpp"
#include "cli/version.hpp"
#include "cli/xbdm.hpp"

#include <format>

namespace updclient::cli {

namespace {

Outcome<void> runInfo(Context &context) {
  if (context.targetsXbdm()) return xbdmInfo(context);
  return withUpdServer(context, "", [&context](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.getInfo();
    if (!result) return fromError(result.error());

    const updserver::NandInfo &info = *result;
    const uint32_t structVersion = info.structVer;
    const uint32_t kernelVersion = info.kernelVer;
    const uint32_t dumpSize = info.dumpSize;
    const uint32_t blockSize = info.blockSize;
    const uint32_t pairing = info.pairing;
    const std::string cpuKey = formatHex(std::span<const uint8_t>(info.cpuKey));
    const std::string dvdKey = formatHex(std::span<const uint8_t>(info.dvdKey));

    nlohmann::json json = {{"kernel_version", kernelVersion},
                           {"struct_version", structVersion},
                           {"dump_size", dumpSize},
                           {"block_size", blockSize},
                           {"pairing", std::format("{:08X}", pairing)},
                           {"cpu_key", cpuKey},
                           {"dvd_key", dvdKey}};
    std::string text;
    text += "================ CONSOLE NAND INFO ================\n";
    text += std::format("  Kernel Version  : {}\n", kernelVersion);
    text += std::format("  NAND Dump Size  : {} MB ({} bytes)\n", dumpSize / (1024 * 1024), dumpSize);
    text += std::format("  Block Size      : {} bytes\n", blockSize);
    text += std::format("  Pairing Data    : {:08X}\n", pairing);
    text += std::format("  CPU Key         : {}\n", cpuKey);
    text += std::format("  DVD Key         : {}\n", dvdKey);
    text += "===================================================\n";
    context.output.result(json, text);
    return {};
  });
}

Outcome<void> runVersion(Context &context) {
  return withUpdServer(context, "", [&context](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.getVersion();
    if (!result) return fromError(result.error());
    context.output.result({{"server_version", *result}, {"client_version", kVersion}},
                          std::format("UpdServer Version: {}\n", *result));
    return {};
  });
}

} // namespace

void registerInfoCommands(CLI::App &app, Context &context) {
  auto *info = app.add_subcommand("info", "Fetch console hardware info, CPU key, DVD key and NAND geometry (UpdServer), "
                                          "or the debug name, type, id and running title (XBDM)");
  info->callback([&context] { context.finish(runInfo(context)); });

  auto *version = app.add_subcommand("version", "Get the UpdServer version running on the console");
  version->callback([&context] { context.finish(runVersion(context)); });
}

} // namespace updclient::cli
