#include "cli/xbdm.hpp"

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

// "/HDD/dir/file" or "HDD:\dir\file".
Outcome<std::string> consolePath(const std::string &text) {
  auto path = !text.empty() && text.front() == '/' ? xbdm::toConsolePath(text) : xbdm::canonicalPath(text);
  if (!path) return usageError("'" + text + "' is not a console path: " + path.error().message);
  return *path;
}

std::string fileTimeText(const std::optional<uint64_t> &fileTime) {
  if (!fileTime || *fileTime == 0) return "-";
  const auto time = xbdm::fileTimeToTimePoint(*fileTime);
  if (!time) return "-";
  return std::format("{:%F %T}", std::chrono::floor<std::chrono::seconds>(*time));
}

nlohmann::json fileTimeJson(const std::optional<uint64_t> &fileTime) {
  if (!fileTime) return nullptr;
  return std::format("0x{:016X}", *fileTime);
}

nlohmann::json attributesJson(const xbdm::FileAttributes &a) {
  return {{"size", a.size},
          {"directory", a.isDirectory},
          {"read_only", a.isReadOnly},
          {"hidden", a.isHidden},
          {"created", fileTimeJson(a.createdFileTime)},
          {"changed", fileTimeJson(a.changedFileTime)}};
}

std::string memoryText(const xbdm::MemoryRead &read) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < read.data.size(); ++i) {
    if (!read.readable[i]) {
      out += "??";
      continue;
    }
    out += digits[read.data[i] >> 4];
    out += digits[read.data[i] & 0xF];
  }
  return out;
}

nlohmann::json powerJson(const std::string &action, const net::Endpoint &endpoint, xbdm::PowerResult result) {
  return {{"action", action},
          {"target", endpoint.toString()},
          {"acknowledged", result == xbdm::PowerResult::Acknowledged}};
}

std::string powerText(const std::string &action, const net::Endpoint &endpoint, xbdm::PowerResult result) {
  return std::format("Sent {} to {} ({})", action, endpoint.toString(),
                     result == xbdm::PowerResult::Acknowledged ? "the console acknowledged it"
                                                               : "the console closed the connection without an answer");
}

Outcome<void> runList(Context &context, const std::string &directory) {
  auto path = consolePath(directory);
  if (!path) return unexpected<Failure>(path.error());
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    auto listing = client.list(*path);
    if (!listing) return fromError(listing.error());
    nlohmann::json entries = nlohmann::json::array();
    std::string text;
    for (const auto &e : listing->entries) {
      auto entry = attributesJson(e);
      entry["name"] = e.name;
      entries.push_back(std::move(entry));
      text += std::format("{} {:>14} {:19} {}\n", e.isDirectory ? 'd' : '-', e.isDirectory ? 0 : e.size,
                          fileTimeText(e.changedFileTime), e.name);
    }
    if (listing->skipped > 0) spdlog::warn("{} entries of the listing could not be read", listing->skipped);
    context.output.result({{"path", *path}, {"entries", entries}, {"skipped", listing->skipped}}, text);
    return {};
  });
}

Outcome<void> runStat(Context &context, const std::string &target) {
  auto path = consolePath(target);
  if (!path) return unexpected<Failure>(path.error());
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    auto a = client.attributes(*path);
    if (!a) return fromError(a.error());
    auto json = attributesJson(*a);
    json["path"] = *path;
    context.output.result(json, std::format("{}\n  type    : {}\n  size    : {} bytes\n  created : {}\n  changed : {}\n",
                                            *path, a->isDirectory ? "folder" : "file", a->size,
                                            fileTimeText(a->createdFileTime), fileTimeText(a->changedFileTime)));
    return {};
  });
}

Outcome<void> runDrives(Context &context) {
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    auto drives = client.drives();
    if (!drives) return fromError(drives.error());
    nlohmann::json list = nlohmann::json::array();
    std::string text;
    for (const auto &name : *drives) {
      nlohmann::json drive = {{"name", name}};
      auto space = client.driveSpace(name);
      if (space) {
        drive["total_bytes"] = space->totalBytes;
        drive["free_bytes"] = space->freeToCaller;
        text += std::format("{:<10} {:>16} total {:>16} free\n", name + ":", space->totalBytes, space->freeToCaller);
      } else if (xbdm::consoleStatusCode(space.error())) {
        text += std::format("{:<10} (no size: {})\n", name + ":", space.error().message);
      } else {
        return fromError(space.error());
      }
      list.push_back(std::move(drive));
    }
    context.output.result({{"drives", list}}, text);
    return {};
  });
}

Outcome<void> runRemove(Context &context, const std::string &target, bool directory) {
  auto path = consolePath(target);
  if (!path) return unexpected<Failure>(path.error());
  const std::string action = std::format("DELETE the {} {}", directory ? "empty folder" : "file", *path);
  return withXbdm(context, action, [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    auto r = directory ? client.removeDirectory(*path) : client.removeFile(*path);
    if (!r) return fromError(r.error());
    context.output.result({{"action", "delete"}, {"path", *path}, {"directory", directory}}, "Deleted " + *path);
    return {};
  });
}

Outcome<void> runMove(Context &context, const std::string &fromText, const std::string &toText) {
  auto from = consolePath(fromText);
  if (!from) return unexpected<Failure>(from.error());
  auto to = consolePath(toText);
  if (!to) return unexpected<Failure>(to.error());
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    auto r = client.rename(*from, *to);
    if (!r) return fromError(r.error());
    context.output.result({{"action", "rename"}, {"from", *from}, {"to", *to}}, "Renamed " + *from + " to " + *to);
    return {};
  });
}

Outcome<void> runScreenshot(Context &context, const std::string &output) {
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    spdlog::info("Asking for a screenshot (the console can take a few seconds)...");
    auto shot = client.screenshot();
    if (!shot) return fromError(shot.error());
    if (auto written = writeFile(pathFromUtf8(output), shot->data); !written) return written;
    nlohmann::json json = {{"output", output},       {"bytes", shot->data.size()}, {"pitch", shot->pitch},
                           {"width", shot->width},   {"height", shot->height},     {"format", shot->format},
                           {"offset_x", shot->offsetX}, {"offset_y", shot->offsetY}, {"tiled", true}};
    context.output.result(json, std::format("Frame buffer written to {} ({} bytes, {}x{}, pitch {}, format 0x{:08X}; "
                                            "raw GPU tiling, not an image file)",
                                            output, shot->data.size(), shot->width, shot->height, shot->pitch,
                                            shot->format));
    return {};
  });
}

Outcome<void> runLaunch(Context &context, const std::string &target) {
  auto path = consolePath(target);
  if (!path) return unexpected<Failure>(path.error());
  return withXbdm(context, "launch " + *path + ", ending the running title",
                  [&](xbdm::XbdmClient &client, const net::Endpoint &endpoint) -> Outcome<void> {
                    auto r = client.launch(*path);
                    if (!r) return fromError(r.error());
                    auto json = powerJson("launch", endpoint, *r);
                    json["title"] = *path;
                    context.output.result(json, powerText("launch of " + *path, endpoint, *r));
                    return {};
                  });
}

Outcome<void> runModules(Context &context) {
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    auto modules = client.modules();
    if (!modules) return fromError(modules.error());
    nlohmann::json list = nlohmann::json::array();
    std::string text;
    for (const auto &m : *modules) {
      list.push_back({{"name", m.name},
                      {"base", std::format("{:08X}", m.base)},
                      {"size", m.size},
                      {"checksum", m.checksum ? nlohmann::json(std::format("{:08X}", *m.checksum)) : nlohmann::json()},
                      {"timestamp", m.timestamp ? nlohmann::json(*m.timestamp) : nlohmann::json()}});
      text += std::format("{:08X} {:>10} {}\n", m.base, m.size, m.name);
    }
    context.output.result({{"modules", list}}, text);
    return {};
  });
}

Outcome<void> runRegions(Context &context) {
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    auto regions = client.memoryRegions();
    if (!regions) return fromError(regions.error());
    nlohmann::json list = nlohmann::json::array();
    std::string text;
    for (const auto &r : *regions) {
      list.push_back({{"base", std::format("{:08X}", r.base)},
                      {"size", r.size},
                      {"protect", std::format("{:08X}", r.protect)},
                      {"phys", std::format("{:08X}", r.phys)}});
      text += std::format("{:08X} {:>10} protect {:08X}\n", r.base, r.size, r.protect);
    }
    context.output.result({{"regions", list}}, text);
    return {};
  });
}

Outcome<void> runRaw(Context &context, const std::string &line) {
  return withXbdm(context, "send the raw command '" + line + "'",
                  [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
                    auto answer = client.rawCommand(line);
                    if (!answer) return fromError(answer.error());
                    std::string text = std::format("{}- {}\n", answer->status.code, answer->status.text);
                    for (const auto &l : answer->body) text += l + "\n";
                    if (answer->status.code == xbdm::status::kMultiline) text += ".\n";
                    context.output.result({{"command", line},
                                           {"status", answer->status.code},
                                           {"text", answer->status.text},
                                           {"body", answer->body}},
                                          text);
                    return {};
                  });
}

Outcome<void> runEject(Context &context) {
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    auto r = client.ejectTray();
    if (!r) return fromError(r.error());
    context.output.result({{"action", "eject"}}, "Opened the tray");
    return {};
  });
}

} // namespace

Outcome<void> xbdmGet(Context &context, const std::string &remote, const std::string &local) {
  auto path = consolePath(remote);
  if (!path) return unexpected<Failure>(path.error());
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    spdlog::info("Downloading '{}' to '{}'...", *path, local);
    Progress progress("Download");
    auto result = client.downloadToFile(*path, pathFromUtf8(local), [&progress](uint64_t done, uint64_t total) {
      progress.update(static_cast<size_t>(done), static_cast<size_t>(total));
    });
    if (!result) return fromError(result.error());
    const uint64_t bytes = fileSizeOrZero(pathFromUtf8(local));
    context.output.result({{"remote", *path}, {"local", local}, {"bytes", bytes}},
                          std::format("Downloaded {} to {} ({} bytes)", *path, local, bytes));
    return {};
  });
}

Outcome<void> xbdmSend(Context &context, const std::string &local, const std::string &remote) {
  auto path = consolePath(remote);
  if (!path) return unexpected<Failure>(path.error());
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    spdlog::info("Uploading '{}' to '{}'...", local, *path);
    Progress progress("Upload");
    auto result = client.uploadFromFile(pathFromUtf8(local), *path, [&progress](uint64_t done, uint64_t total) {
      progress.update(static_cast<size_t>(done), static_cast<size_t>(total));
    });
    if (!result) return fromError(result.error());
    const uint64_t bytes = fileSizeOrZero(pathFromUtf8(local));
    context.output.result({{"local", local}, {"remote", *path}, {"bytes", bytes}, {"acknowledged", true}},
                          std::format("Uploaded {} to {} ({} bytes, confirmed by the console)", local, *path, bytes));
    return {};
  });
}

Outcome<void> xbdmMkdir(Context &context, const std::string &remote) {
  auto path = consolePath(remote);
  if (!path) return unexpected<Failure>(path.error());
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.makeDirectory(*path);
    if (!result) return fromError(result.error());
    context.output.result({{"action", "mkdir"}, {"path", *path}, {"acknowledged", true}}, "Created " + *path);
    return {};
  });
}

Outcome<void> xbdmInfo(Context &context) {
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &endpoint) -> Outcome<void> {
    auto info = client.consoleInfo();
    if (!info) return fromError(info.error());
    auto optional = [](const std::optional<std::string> &value) {
      return value ? nlohmann::json(*value) : nlohmann::json();
    };
    nlohmann::json json = {{"target", endpoint.toString()},
                           {"debug_name", optional(info->debugName)},
                           {"console_type", optional(info->consoleType)},
                           {"console_id", optional(info->consoleId)},
                           {"running_title", info->runningTitle ? nlohmann::json(info->runningTitle->name) : nlohmann::json()},
                           {"exec_state", info->execState ? nlohmann::json(info->execState->text) : nlohmann::json()},
                           {"title_address", info->titleAddress ? nlohmann::json(info->titleAddress->text) : nlohmann::json()}};
    auto shown = [](const std::optional<std::string> &value) { return value ? *value : std::string("(not answered)"); };
    std::string text;
    text += "================ XBDM CONSOLE INFO ================\n";
    text += std::format("  Debug Name      : {}\n", shown(info->debugName));
    text += std::format("  Console Type    : {}\n", shown(info->consoleType));
    text += std::format("  Console ID      : {}\n", shown(info->consoleId));
    text += std::format("  Running Title   : {}\n", info->runningTitle ? info->runningTitle->name : "(not answered)");
    text += std::format("  Execution State : {}\n", info->execState ? info->execState->text : "(not answered)");
    text += std::format("  Title Address   : {}\n", info->titleAddress ? info->titleAddress->text : "(not answered)");
    text += "===================================================\n";
    context.output.result(json, text);
    return {};
  });
}

Outcome<void> xbdmPeek(Context &context, uint32_t address, uint32_t length) {
  return withXbdm(context, "", [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    auto read = client.getMemoryEx(address, length);
    // getmemex is in two references only; getmem is the fallback (spec 3.16).
    if (!read && xbdm::consoleStatusCode(read.error()) == xbdm::status::kInvalidCommand) {
      read = client.getMemory(address, length);
    }
    if (!read) return fromError(read.error());
    const std::string hex = memoryText(*read);
    context.output.result({{"address", std::format("{:08X}", address)},
                           {"length", length},
                           {"data", hex},
                           {"readable_bytes", read->readableBytes()}},
                          std::format("PEEK 0x{:08X} ({} bytes, {} readable): {}", address, length,
                                      read->readableBytes(), hex));
    return {};
  });
}

Outcome<void> xbdmPoke(Context &context, uint32_t address, uint32_t value) {
  const std::string action = std::format("WRITE 0x{:08X} (big-endian) to memory at 0x{:08X}", value, address);
  return withXbdm(context, action, [&](xbdm::XbdmClient &client, const net::Endpoint &) -> Outcome<void> {
    const uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                              static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    auto result = client.setMemory(address, bytes);
    if (!result) return fromError(result.error());
    context.output.result({{"action", "poke"},
                           {"address", std::format("{:08X}", address)},
                           {"value", std::format("{:08X}", value)},
                           {"acknowledged", true}},
                          std::format("Wrote 0x{:08X} at 0x{:08X} (confirmed by the console)", value, address));
    return {};
  });
}

Outcome<void> xbdmReboot(Context &context, bool cold) {
  const std::string name = cold ? "cold reboot" : "reboot";
  return withXbdm(context, cold ? "cold-reboot the console" : "reboot the console",
                  [&](xbdm::XbdmClient &client, const net::Endpoint &endpoint) -> Outcome<void> {
                    auto r = client.reboot(cold ? xbdm::RebootMode::Cold : xbdm::RebootMode::Warm);
                    if (!r) return fromError(r.error());
                    context.output.result(powerJson(name, endpoint, *r), powerText(name, endpoint, *r));
                    return {};
                  });
}

Outcome<void> xbdmShutdown(Context &context) {
  return withXbdm(context, "shut the console down",
                  [&](xbdm::XbdmClient &client, const net::Endpoint &endpoint) -> Outcome<void> {
                    auto r = client.shutdown();
                    if (!r) return fromError(r.error());
                    context.output.result(powerJson("shutdown", endpoint, *r), powerText("shutdown", endpoint, *r));
                    return {};
                  });
}

void registerXbdmCommands(CLI::App &app, Context &context) {
  auto *group = addGroup(app, "xbdm",
                         "Xbox debug monitor operations (--target xbdm://host, a bare host, or the first console "
                         "XBDM discovery finds)");

  auto path = std::make_shared<std::string>();
  auto second = std::make_shared<std::string>();
  auto flag = std::make_shared<bool>(false);
  auto output = std::make_shared<std::string>("screenshot.raw");

  auto *ls = group->add_subcommand("ls", "List a folder on the console");
  ls->add_option("path", *path, "Console folder, e.g. HDD:\\ or /HDD/Content")->required();
  ls->callback([&context, path] { context.finish(runList(context, *path)); });

  auto *stat = group->add_subcommand("stat", "Show the size and times of a file or folder");
  stat->add_option("path", *path, "Console path")->required();
  stat->callback([&context, path] { context.finish(runStat(context, *path)); });

  auto *drives = group->add_subcommand("drives", "List the drives and their free space");
  drives->callback([&context] { context.finish(runDrives(context)); });

  auto *rm = group->add_subcommand("rm", "Delete a file, or an empty folder with --dir (destructive: needs --yes or confirmation)");
  rm->add_option("path", *path, "Console path")->required();
  rm->add_flag("--dir", *flag, "The path is an empty folder");
  rm->callback([&context, path, flag] { context.finish(runRemove(context, *path, *flag)); });

  auto *mv = group->add_subcommand("mv", "Rename or move a file or folder within one drive");
  mv->add_option("from", *path, "Current console path")->required();
  mv->add_option("to", *second, "New console path; must not exist")->required();
  mv->callback([&context, path, second] { context.finish(runMove(context, *path, *second)); });

  auto *shot = group->add_subcommand("screenshot", "Save the raw (tiled) frame buffer and print its geometry");
  shot->add_option("-o,--output", *output, "Output file path")->capture_default_str();
  shot->callback([&context, output] { context.finish(runScreenshot(context, *output)); });

  auto *launch = group->add_subcommand("launch", "Start an executable with magicboot (destructive: needs --yes or confirmation)");
  launch->add_option("path", *path, "Console path of the .xex")->required();
  launch->callback([&context, path] { context.finish(runLaunch(context, *path)); });

  auto *reboot = group->add_subcommand("reboot", "Reboot with magicboot, warm or --cold (destructive: needs --yes or confirmation)");
  reboot->add_flag("--cold", *flag, "Cold reboot");
  reboot->callback([&context, flag] { context.finish(xbdmReboot(context, *flag)); });

  auto *modules = group->add_subcommand("modules", "List the loaded modules");
  modules->callback([&context] { context.finish(runModules(context)); });

  auto *regions = group->add_subcommand("regions", "List the committed memory regions (walkmem)");
  regions->callback([&context] { context.finish(runRegions(context)); });

  auto *raw = group->add_subcommand("raw", "Send one command line as typed and print the answer, for diagnostics; "
                                            "binary answers are not read (needs --yes or confirmation)");
  raw->add_option("line", *path, "The command line, e.g. 'dirlist name=\"HDD:\"'")->required();
  raw->callback([&context, path] { context.finish(runRaw(context, *path)); });

  auto *eject = group->add_subcommand("eject", "Open the disc tray");
  eject->callback([&context] { context.finish(runEject(context)); });
}

} // namespace updclient::cli
