#include "cli/commands.hpp"
#include "cli/args.hpp"
#include "cli/fileio.hpp"
#include "cli/progress.hpp"
#include "cli/session.hpp"

#include <spdlog/spdlog.h>

#include <format>
#include <memory>

namespace updclient::cli {

namespace {

struct FileArgs {
  std::string first;
  std::string second;
};

Outcome<void> runGet(Context &context, const FileArgs &args) {
  return withUpdServer(context, "", [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    spdlog::info("Downloading '{}' to '{}'...", args.first, args.second);
    Progress progress("Download");
    auto result = client.getFile(args.first, pathFromUtf8(args.second), [&progress](size_t done) { progress.update(done, 0); });
    if (!result) return fromError(result.error());
    const uint64_t bytes = fileSizeOrZero(pathFromUtf8(args.second));
    context.output.result({{"remote", args.first}, {"local", args.second}, {"bytes", bytes}},
                          std::format("Downloaded {} to {} ({} bytes)", args.first, args.second, bytes));
    return {};
  });
}

Outcome<void> runSend(Context &context, const FileArgs &args) {
  return withUpdServer(context, "", [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    spdlog::info("Uploading '{}' to '{}'...", args.first, args.second);
    Progress progress("Upload");
    auto result = client.sendFile(pathFromUtf8(args.first), args.second, [&progress](size_t done) { progress.update(done, 0); });
    if (!result) return fromError(result.error());
    const uint64_t bytes = fileSizeOrZero(pathFromUtf8(args.first));
    context.output.result({{"local", args.first}, {"remote", args.second}, {"bytes", bytes}, {"acknowledged", false}},
                          std::format("Uploaded {} to {} ({} bytes sent)", args.first, args.second, bytes));
    return {};
  });
}

Outcome<void> runMount(Context &context, const FileArgs &args) {
  return withUpdServer(context, "", [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.mount(args.first, args.second);
    if (!result) return fromError(result.error());
    context.output.result({{"action", "mount"}, {"mount_point", args.first}, {"device", args.second}, {"acknowledged", false}},
                          std::format("Sent mount of {} at {} (not acknowledged)", args.second, args.first));
    return {};
  });
}

Outcome<void> runUnmount(Context &context, const FileArgs &args) {
  return withUpdServer(context, "", [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.unmount(args.first);
    if (!result) return fromError(result.error());
    context.output.result({{"action", "unmount"}, {"mount_point", args.first}, {"acknowledged", false}},
                          std::format("Sent unmount of {} (not acknowledged)", args.first));
    return {};
  });
}

Outcome<void> runMkdir(Context &context, const FileArgs &args) {
  return withUpdServer(context, "", [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.mkDir(args.first);
    if (!result) return fromError(result.error());
    context.output.result({{"action", "mkdir"}, {"path", args.first}, {"acknowledged", false}},
                          std::format("Sent mkdir of {} (not acknowledged)", args.first));
    return {};
  });
}

using Runner = Outcome<void> (*)(Context &, const FileArgs &);

void addFileCommand(CLI::App *group, Context &context, const std::string &name, const std::string &description,
                    const char *firstName, const char *firstHelp, const char *secondName, const char *secondHelp,
                    Runner runner) {
  auto args = std::make_shared<FileArgs>();
  auto *command = group->add_subcommand(name, description);
  command->add_option(firstName, args->first, firstHelp)->required();
  if (secondName != nullptr) command->add_option(secondName, args->second, secondHelp)->required();
  command->callback([&context, args, runner] { context.finish(runner(context, *args)); });
}

} // namespace

void registerFileCommands(CLI::App &app, Context &context) {
  auto *file = addGroup(app, "file", "Console storage operations (UpdServer)");
  addFileCommand(file, context, "get", "Download a file from the console", "remote", "Remote path on the console",
                 "local", "Local destination file path", runGet);
  addFileCommand(file, context, "send", "Upload a file to the console", "local", "Local source file path", "remote",
                 "Remote destination path on the console", runSend);
  addFileCommand(file, context, "mount", "Mount a device at a mount point on the console", "mount-point",
                 "Mount point name (no spaces)", "device", "Device path to mount", runMount);
  addFileCommand(file, context, "unmount", "Unmount a mount point on the console", "mount-point",
                 "Mount point name", nullptr, nullptr, runUnmount);
  addFileCommand(file, context, "mkdir", "Create a directory on the console", "path", "Remote directory path", nullptr,
                 nullptr, runMkdir);
}

} // namespace updclient::cli
