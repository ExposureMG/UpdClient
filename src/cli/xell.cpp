#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "cli/fileio.hpp"
#include "cli/progress.hpp"
#include "cli/session.hpp"

#include <spdlog/spdlog.h>

#include <format>
#include <memory>

namespace updclient::cli {

namespace {

struct XellArgs {
  std::string flashOutput = "xell_flash.bin";
  std::string kvOutput = "kv.bin";
  bool raw = false;
  bool rawBlock = false;
};

nlohmann::json optionalString(const std::string &value) {
  return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
}

Outcome<void> runInfo(Context &context) {
  return withXell(context, [&](const xell::XellClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.getInfo();
    if (!result) return fromError(result.error());
    const xell::XellInfo &info = *result;

    const std::string cpuKey = info.cpuKeyValid ? formatHex(info.cpuKey) : "UNKNOWN";
    const std::string dvdKey = info.dvdKeyValid ? formatHex(info.dvdKey) : "UNKNOWN";
    nlohmann::json json = {{"cpu_key", info.cpuKeyValid ? nlohmann::json(cpuKey) : nlohmann::json(nullptr)},
                           {"dvd_key", info.dvdKeyValid ? nlohmann::json(dvdKey) : nlohmann::json(nullptr)},
                           {"bg_color", optionalString(info.bgColor)},
                           {"fg_color", optionalString(info.fgColor)},
                           {"missing_fields", info.missingFields()}};

    std::string text;
    text += "================ XELL CONSOLE INFO ================\n";
    text += std::format("  CPU Key         : {}\n", cpuKey);
    text += std::format("  DVD Key         : {}\n", dvdKey);
    text += std::format("  Background Color: {}\n", info.bgColor.empty() ? "UNKNOWN" : info.bgColor);
    text += std::format("  Foreground Color: {}\n", info.fgColor.empty() ? "UNKNOWN" : info.fgColor);
    text += "===================================================\n";
    context.output.result(json, text);
    return {};
  });
}

Outcome<void> runFlashDump(Context &context, const XellArgs &args) {
  return withXell(context, [&](const xell::XellClient &client, const net::Endpoint &) -> Outcome<void> {
    spdlog::info("Downloading the NAND image from XeLL to '{}'...", args.flashOutput);
    Progress progress("XeLL flash dump");
    auto result = client.dumpFlash(pathFromUtf8(args.flashOutput), [&progress](size_t done, size_t total) { progress.update(done, total); });
    if (!result) return fromError(result.error());
    const uint64_t bytes = fileSizeOrZero(pathFromUtf8(args.flashOutput));
    context.output.result({{"output", args.flashOutput}, {"bytes", bytes}},
                          std::format("XeLL flash dump written to {} ({} bytes)", args.flashOutput, bytes));
    return {};
  });
}

Outcome<void> runFuses(Context &context) {
  return withXell(context, [&](const xell::XellClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.getFuses();
    if (!result) return fromError(result.error());
    context.output.result({{"fuses_raw", *result}}, *result);
    return {};
  });
}

Outcome<void> runKeyvault(Context &context, const XellArgs &args) {
  const char *modeName = args.rawBlock ? "raw-block" : (args.raw ? "raw" : "decrypted");
  const auto mode = args.rawBlock ? xell::KeyvaultMode::RawBlock
                                  : (args.raw ? xell::KeyvaultMode::Raw : xell::KeyvaultMode::Decrypted);
  return withXell(context, [&](const xell::XellClient &client, const net::Endpoint &) -> Outcome<void> {
    spdlog::info("Downloading the {} keyvault from XeLL...", modeName);
    auto result = client.getKeyvault(mode);
    if (!result) return fromError(result.error());
    if (auto written = writeFile(pathFromUtf8(args.kvOutput), *result); !written) return written;
    context.output.result({{"output", args.kvOutput}, {"bytes", result->size()}, {"mode", modeName}},
                          std::format("Keyvault ({}) written to {} ({} bytes)", modeName, args.kvOutput, result->size()));
    return {};
  });
}

} // namespace

void registerXellCommands(CLI::App &app, Context &context) {
  auto *xellGroup = addGroup(app, "xell", "XeLL Reloaded HTTPD operations (needs --target or --ip; default port 80)");
  auto args = std::make_shared<XellArgs>();

  auto *info = xellGroup->add_subcommand("info", "Fetch the CPU key, DVD key and XeLL colors from the HTTPD page");
  info->callback([&context] { context.finish(runInfo(context)); });

  auto *flash = xellGroup->add_subcommand("flash-dump", "Download the NAND flash image from XeLL");
  flash->add_option("-o,--output", args->flashOutput, "Output file path")->capture_default_str();
  flash->callback([&context, args] { context.finish(runFlashDump(context, *args)); });

  auto *fuses = xellGroup->add_subcommand("fuses", "Get the console fuse listing from XeLL");
  fuses->callback([&context] { context.finish(runFuses(context)); });

  auto *kv = xellGroup->add_subcommand("kv", "Download the keyvault from XeLL");
  kv->add_option("-o,--output", args->kvOutput, "Output file path")->capture_default_str();
  auto *raw = kv->add_flag("-r,--raw", args->raw, "Download the raw keyvault (/KVRAW) instead of the decrypted one");
  auto *rawBlock = kv->add_flag("--raw-block", args->rawBlock, "Download the raw keyvault block (/KVRAW2)");
  raw->excludes(rawBlock);
  kv->callback([&context, args] { context.finish(runKeyvault(context, *args)); });
}

} // namespace updclient::cli
