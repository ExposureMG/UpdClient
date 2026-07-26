#include "updclient/tcp_client.hpp"
#include "updclient/udp_discovery.hpp"
#include "updclient/protocol.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <format>
#include <fstream>

using json = nlohmann::json;

namespace updclient {

std::string resolveTargetIp(const std::string& explicitIp) {
    if (!explicitIp.empty()) {
        spdlog::debug("Using explicitly provided IP: {}", explicitIp);
        return explicitIp;
    }

    spdlog::info("No IP address specified. Auto-discovering Xbox 360 consoles via UDP port {}...", ANNC_PORT);
    auto discoveredIp = UdpDiscovery::discoverOne(std::chrono::milliseconds(3000));
    if (discoveredIp) {
        spdlog::info("Auto-connected to discovered console at {}", *discoveredIp);
        return *discoveredIp;
    }

    spdlog::error("Auto-discovery timed out. No Xbox 360 consoles found on the local network.");
    spdlog::error("Please specify target console IP explicitly using --ip <IP>");
    return {};
}

} // namespace updclient

int main(int argc, char** argv) {
    CLI::App app{"UpdClient - Modern C++23 XeBuild/DashLaunch UpdServer Client"};

    std::string ipAddress;
    uint16_t port = updclient::NANDSVR_PORT;
    bool jsonOutput = false;
    bool verbose = false;

    app.add_option("-i,--ip", ipAddress, "Target Xbox 360 IP address (default: auto-connect via UDP discovery)");
    app.add_option("-p,--port", port, "Target UpdServer TCP port")->capture_default_str();
    app.add_flag("-j,--json", jsonOutput, "Output results in JSON format");
    app.add_flag("-v,--verbose", verbose, "Enable verbose debug logging");

    // 1. DISCOVER
    auto* subDiscover = app.add_subcommand("discover", "Auto-discover Xbox 360 consoles broadcasting on UDP port 48");

    // 2. INFO
    auto* subInfo = app.add_subcommand("info", "Fetch console hardware info, CPU key, DVD key, and fuses");

    // 3. VERSION
    auto* subVersion = app.add_subcommand("version", "Get UpdServer server version");

    // 4. POWER SUBCOMMANDS
    auto* subPower = app.add_subcommand("power", "Console power management commands");
    auto* subReboot = subPower->add_subcommand("reboot", "Standard software reboot console");
    auto* subSmcReset = subPower->add_subcommand("smc-reset", "Hardware SMC reset console");
    auto* subShutdown = subPower->add_subcommand("shutdown", "Shutdown console");

    // 5. NAND SUBCOMMANDS
    auto* subNand = app.add_subcommand("nand", "NAND flash operations");
    
    auto* subNandDump = subNand->add_subcommand("dump", "Dump full NAND flash image");
    std::string nandOutPath = "nanddump.bin";
    subNandDump->add_option("-o,--output", nandOutPath, "Output file path")->capture_default_str();

    auto* subNandBb = subNand->add_subcommand("badblocks", "Get list of bad blocks on console NAND");

    auto* subNandReadBlk = subNand->add_subcommand("read-block", "Read raw block from NAND");
    uint32_t readBlkIdx = 0;
    uint32_t readBlkCount = 1;
    subNandReadBlk->add_option("block", readBlkIdx, "Block index (hex or dec)")->required();
    subNandReadBlk->add_option("count", readBlkCount, "Number of blocks to read")->capture_default_str();

    auto* subNandEraseBlk = subNand->add_subcommand("erase-block", "Erase raw block on NAND");
    uint32_t eraseBlkIdx = 0;
    uint32_t eraseBlkCount = 1;
    subNandEraseBlk->add_option("block", eraseBlkIdx, "Block index (hex or dec)")->required();
    subNandEraseBlk->add_option("count", eraseBlkCount, "Number of blocks to erase")->capture_default_str();

    // 6. MEMORY SUBCOMMANDS
    auto* subMem = app.add_subcommand("mem", "Memory peek/poke and hypervisor operations");
    
    auto* subPeek = subMem->add_subcommand("peek", "Peek physical memory");
    uint32_t peekAddr = 0, peekLen = 16;
    subPeek->add_option("address", peekAddr, "Memory address (hex)")->required();
    subPeek->add_option("length", peekLen, "Length in bytes")->capture_default_str();

    auto* subPoke = subMem->add_subcommand("poke", "Poke physical memory");
    uint32_t pokeAddr = 0, pokeVal = 0;
    subPoke->add_option("address", pokeAddr, "Memory address (hex)")->required();
    subPoke->add_option("value", pokeVal, "32-bit value (hex)")->required();

    auto* subHvPeek = subMem->add_subcommand("hvpeek", "Peek Hypervisor (HV) memory space");
    uint64_t hvPeekAddr = 0;
    uint32_t hvPeekLen = 16;
    subHvPeek->add_option("address", hvPeekAddr, "HV address (hex)")->required();
    subHvPeek->add_option("length", hvPeekLen, "Length in bytes")->capture_default_str();

    auto* subHvPoke = subMem->add_subcommand("hvpoke", "Poke Hypervisor (HV) memory space");
    uint64_t hvPokeAddr = 0, hvPokeVal = 0;
    subHvPoke->add_option("address", hvPokeAddr, "HV address (hex)")->required();
    subHvPoke->add_option("value", hvPokeVal, "64-bit value (hex)")->required();

    auto* subGet1Bl = subMem->add_subcommand("get1bl", "Dump 1BL ROM (32KB)");
    std::string blOutPath = "1bl.bin";
    subGet1Bl->add_option("-o,--output", blOutPath, "Output file path")->capture_default_str();

    // 7. FILE SUBCOMMANDS
    auto* subFile = app.add_subcommand("file", "Storage file operations");
    
    auto* subFileGet = subFile->add_subcommand("get", "Download file from console");
    std::string remoteGetPath, localGetPath;
    subFileGet->add_option("remote", remoteGetPath, "Remote path on console")->required();
    subFileGet->add_option("local", localGetPath, "Local destination file path")->required();

    auto* subFileSend = subFile->add_subcommand("send", "Upload file to console");
    std::string localSendPath, remoteSendPath;
    subFileSend->add_option("local", localSendPath, "Local source file path")->required();
    subFileSend->add_option("remote", remoteSendPath, "Remote destination path on console")->required();

    CLI11_PARSE(app, argc, argv);

    if (verbose) {
        spdlog::set_level(spdlog::level::debug);
    } else {
        spdlog::set_level(spdlog::level::info);
    }

    // 1. DISCOVER SUBCOMMAND
    if (*subDiscover) {
        auto consoles = updclient::UdpDiscovery::discoverAll(std::chrono::milliseconds(3000));
        if (jsonOutput) {
            json jList = json::array();
            for (const auto& c : consoles) {
                jList.push_back({{"ip", c.ipAddress}});
            }
            std::cout << jList.dump(4) << "\n";
        } else {
            spdlog::info("Discovered {} console(s):", consoles.size());
            for (const auto& c : consoles) {
                std::cout << "  - Console IP: " << c.ipAddress << "\n";
            }
        }
        return 0;
    }

    // RESOLVE TARGET IP FOR ALL OTHER COMMANDS (Autoconnect with IP override)
    std::string targetIp = updclient::resolveTargetIp(ipAddress);
    if (targetIp.empty()) {
        return 1;
    }

    updclient::TcpClient client;
    if (!client.connect(targetIp, port)) {
        return 1;
    }

    // 2. INFO
    if (*subInfo) {
        auto res = client.getInfo();
        if (!res) {
            spdlog::error("Error: {}", res.error());
            return 1;
        }
        const auto& info = res.value();
        std::string cpuKeyStr = updclient::format_hex_bytes(info.cpuKey, 16);
        std::string dvdKeyStr = updclient::format_hex_bytes(info.dvdKey, 16);

        if (jsonOutput) {
            json j;
            j["kernel_version"] = info.kernelVer;
            j["struct_version"] = info.structVer;
            j["dump_size"] = info.dumpSize;
            j["block_size"] = info.blockSize;
            j["pairing"] = std::format("{:08X}", info.pairing);
            j["cpu_key"] = cpuKeyStr;
            j["dvd_key"] = dvdKeyStr;
            std::cout << j.dump(4) << "\n";
        } else {
            std::cout << "================ CONSOLE NAND INFO ================\n";
            std::cout << std::format("  Kernel Version  : {}\n", info.kernelVer);
            std::cout << std::format("  NAND Dump Size  : {} MB ({} bytes)\n", info.dumpSize / (1024 * 1024), info.dumpSize);
            std::cout << std::format("  Block Size      : {} bytes\n", info.blockSize);
            std::cout << std::format("  Pairing Data    : {:08X}\n", info.pairing);
            std::cout << std::format("  CPU Key         : {}\n", cpuKeyStr);
            std::cout << std::format("  DVD Key         : {}\n", dvdKeyStr);
            std::cout << "===================================================\n";
        }
        return 0;
    }

    // 3. VERSION
    if (*subVersion) {
        auto res = client.getVersion();
        if (!res) {
            spdlog::error("Error: {}", res.error());
            return 1;
        }
        if (jsonOutput) {
            json j = {{"server_version", res.value()}};
            std::cout << j.dump(4) << "\n";
        } else {
            spdlog::info("UpdServer Version: {}", res.value());
        }
        return 0;
    }

    // 4. POWER
    if (*subReboot) {
        (void)client.reboot();
        spdlog::info("Sent reboot command to {}", targetIp);
        return 0;
    }
    if (*subSmcReset) {
        (void)client.smcReboot();
        spdlog::info("Sent SMC reset command to {}", targetIp);
        return 0;
    }
    if (*subShutdown) {
        (void)client.shutdownConsole();
        spdlog::info("Sent shutdown command to {}", targetIp);
        return 0;
    }

    // 5. NAND DUMP
    if (*subNandDump) {
        auto infoRes = client.getInfo();
        if (!infoRes) {
            spdlog::error("Failed to fetch info for dump size: {}", infoRes.error());
            return 1;
        }
        size_t dumpSz = infoRes->dumpSize;
        spdlog::info("Starting full NAND dump of size {} bytes to '{}'...", dumpSz, nandOutPath);

        auto dumpRes = client.dumpFlash(nandOutPath, dumpSz, [](size_t read, size_t total) {
            float pct = (static_cast<float>(read) / total) * 100.0f;
            spdlog::info("Progress: {:.1f}% ({}/{} bytes)", pct, read, total);
        });

        if (!dumpRes) {
            spdlog::error("NAND dump failed: {}", dumpRes.error());
            return 1;
        }
        spdlog::info("NAND dump completed successfully: {}", nandOutPath);
        return 0;
    }

    // 5b. BAD BLOCKS
    if (*subNandBb) {
        auto res = client.getBadBlockList();
        if (!res) {
            spdlog::error("Error: {}", res.error());
            return 1;
        }
        const auto& bbList = res.value();
        if (jsonOutput) {
            json j = {{"bad_blocks", bbList}};
            std::cout << j.dump(4) << "\n";
        } else {
            spdlog::info("Bad Block Count: {}", bbList.size());
            for (auto bb : bbList) {
                std::cout << std::format("  - Bad Block Index: 0x{:04X} ({})\n", bb, bb);
            }
        }
        return 0;
    }

    // 6. MEMORY PEEK / HVPEEK
    if (*subPeek) {
        auto res = client.peek(peekAddr, peekLen);
        if (!res) {
            spdlog::error("Error: {}", res.error());
            return 1;
        }
        std::string hexData = updclient::format_hex_bytes(res->data(), res->size());
        if (jsonOutput) {
            json j = {{"address", std::format("{:08X}", peekAddr)}, {"data", hexData}};
            std::cout << j.dump(4) << "\n";
        } else {
            spdlog::info("PEEK 0x{:08X} ({} bytes): {}", peekAddr, peekLen, hexData);
        }
        return 0;
    }

    if (*subHvPeek) {
        auto res = client.hvPeek(hvPeekAddr, hvPeekLen);
        if (!res) {
            spdlog::error("Error: {}", res.error());
            return 1;
        }
        std::string hexData = updclient::format_hex_bytes(res->data(), res->size());
        if (jsonOutput) {
            json j = {{"hv_address", std::format("{:016X}", hvPeekAddr)}, {"data", hexData}};
            std::cout << j.dump(4) << "\n";
        } else {
            spdlog::info("HVPEEK 0x{:016X} ({} bytes): {}", hvPeekAddr, hvPeekLen, hexData);
        }
        return 0;
    }

    // 7. FILE GET / SEND
    if (*subFileGet) {
        spdlog::info("Downloading remote file '{}' to '{}'...", remoteGetPath, localGetPath);
        auto res = client.getFile(remoteGetPath, localGetPath);
        if (!res) {
            spdlog::error("File download failed: {}", res.error());
            return 1;
        }
        spdlog::info("File download complete: {}", localGetPath);
        return 0;
    }

    if (*subFileSend) {
        spdlog::info("Uploading local file '{}' to '{}'...", localSendPath, remoteSendPath);
        auto res = client.sendFile(localSendPath, remoteSendPath);
        if (!res) {
            spdlog::error("File upload failed: {}", res.error());
            return 1;
        }
        spdlog::info("File upload complete: {}", remoteSendPath);
        return 0;
    }

    spdlog::info("Use --help to view available options and subcommands.");
    return 0;
}
