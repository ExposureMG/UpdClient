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

struct DumpArgs {
  std::string output = "nanddump.bin";
};

struct BlockArgs {
  uint32_t block = 0;
  uint32_t count = 1;
  std::string output;
  std::string file;
};

Outcome<void> runDump(Context &context, const DumpArgs &args) {
  return withUpdServer(context, "", [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto info = client.getInfo();
    if (!info) return fromError(info.error());
    const size_t dumpSize = info->dumpSize;

    spdlog::info("Dumping {} bytes of NAND to '{}'...", dumpSize, args.output);
    Progress progress("NAND dump");
    auto result = client.dumpFlash(pathFromUtf8(args.output), dumpSize,
                                   [&progress](size_t done, size_t total) { progress.update(done, total); });
    if (!result) return fromError(result.error());

    context.output.result({{"output", args.output}, {"bytes", dumpSize}},
                          std::format("NAND dump written to {} ({} bytes)", args.output, dumpSize));
    return {};
  });
}

Outcome<void> runBadBlocks(Context &context) {
  return withUpdServer(context, "", [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.getBadBlockList();
    if (!result) return fromError(result.error());

    std::string text = std::format("Bad Block Count: {}\n", result->size());
    for (const uint16_t block : *result) {
      text += std::format("  - Bad Block Index: 0x{:04X} ({})\n", block, block);
    }
    context.output.result({{"bad_blocks", *result}}, text);
    return {};
  });
}

Outcome<void> runReadBlock(Context &context, const BlockArgs &args) {
  return withUpdServer(context, "", [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto data = client.readBlock(args.block, args.count);
    if (!data) return fromError(data.error());

    nlohmann::json json = {{"block", args.block}, {"count", args.count}, {"bytes", data->size()}};
    if (args.output.empty()) {
      json["data"] = formatHex(*data);
      context.output.result(json, hexDump(*data));
      return {};
    }

    if (auto written = writeFile(pathFromUtf8(args.output), *data); !written) return written;
    json["output"] = args.output;
    context.output.result(json, std::format("Read {} block(s) from 0x{:X}: wrote {} bytes to {}", args.count,
                                            args.block, data->size(), args.output));
    return {};
  });
}

Outcome<void> runEraseBlock(Context &context, const BlockArgs &args) {
  const std::string action =
      std::format("ERASE {} NAND block(s) starting at block 0x{:X}", args.count, args.block);
  return withUpdServer(context, action, [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    auto result = client.eraseBlock(args.block, args.count);
    if (!result) return fromError(result.error());

    context.output.result({{"action", "erase-block"}, {"block", args.block}, {"count", args.count}, {"acknowledged", false}},
                          std::format("Sent erase for {} block(s) starting at 0x{:X} (the console does not "
                                      "acknowledge it; verify with 'nand read-block')",
                                      args.count, args.block));
    return {};
  });
}

Outcome<void> runWriteBlock(Context &context, const BlockArgs &args) {
  auto data = readFile(pathFromUtf8(args.file), updserver::kMaxBlockWriteBytes);
  if (!data) return unexpected<Failure>(data.error());

  const std::string action = std::format("WRITE {} bytes from '{}' over NAND block 0x{:X}", data->size(),
                                         args.file, args.block);
  return withUpdServer(context, action, [&](updserver::UpdServerClient &client, const net::Endpoint &) -> Outcome<void> {
    // The block size reported by getInfo lets the client reject a wrongly sized file.
    auto info = client.getInfo();
    if (!info) return fromError(info.error());

    auto result = client.writeBlock(args.block, *data);
    if (!result) return fromError(result.error());

    context.output.result({{"action", "write-block"}, {"block", args.block}, {"bytes", data->size()}, {"acknowledged", false}},
                          std::format("Sent {} bytes to block 0x{:X} (the console does not acknowledge it; "
                                      "verify with 'nand read-block')",
                                      data->size(), args.block));
    return {};
  });
}

} // namespace

void registerNandCommands(CLI::App &app, Context &context) {
  auto *nand = addGroup(app, "nand", "NAND flash operations (UpdServer)");

  auto dumpArgs = std::make_shared<DumpArgs>();
  auto *dump = nand->add_subcommand("dump", "Dump the full NAND flash image to a file");
  dump->add_option("-o,--output", dumpArgs->output, "Output file path")->capture_default_str();
  dump->callback([&context, dumpArgs] { context.finish(runDump(context, *dumpArgs)); });

  auto *badBlocks = nand->add_subcommand("badblocks", "List the bad blocks of the console NAND");
  badBlocks->callback([&context] { context.finish(runBadBlocks(context)); });

  auto readArgs = std::make_shared<BlockArgs>();
  auto *read = nand->add_subcommand("read-block", "Read raw blocks from NAND; hex dump to stdout unless -o is given");
  addNumber(read, "block", readArgs->block, "Block index")->required();
  addNumber(read, "count", readArgs->count, "Number of blocks to read")->default_str("1");
  read->add_option("-o,--output", readArgs->output, "Write the raw bytes to this file instead of a hex dump");
  read->callback([&context, readArgs] { context.finish(runReadBlock(context, *readArgs)); });

  auto eraseArgs = std::make_shared<BlockArgs>();
  auto *erase = nand->add_subcommand("erase-block", "Erase raw blocks on NAND (destructive: needs --yes or confirmation)");
  addNumber(erase, "block", eraseArgs->block, "Block index")->required();
  addNumber(erase, "count", eraseArgs->count, "Number of blocks to erase")->default_str("1");
  erase->callback([&context, eraseArgs] { context.finish(runEraseBlock(context, *eraseArgs)); });

  auto writeArgs = std::make_shared<BlockArgs>();
  auto *write = nand->add_subcommand("write-block", "Write one raw block to NAND from a file (destructive: needs --yes or confirmation)");
  addNumber(write, "block", writeArgs->block, "Block index")->required();
  write->add_option("file", writeArgs->file, "File holding exactly one block of data")->required();
  write->callback([&context, writeArgs] { context.finish(runWriteBlock(context, *writeArgs)); });
}

} // namespace updclient::cli
