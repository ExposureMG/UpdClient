# UpdClient

Modern C++23 CMake command-line client for **UpdServer** (xeBuild / DashLaunch update server on Xbox 360).

Built with **CLI11** for CLI subcommand parsing, **spdlog** for logging, and **nlohmann_json** for structured JSON output.

## Features

- **C++23 Standard**: Uses modern C++23 features including `std::byteswap` and `std::expected`.
- **Auto-Discovery**: Listens for UDP broadcast announcements on port 48 (`ANNC_PORT`).
- **Autoconnect with IP Override**: Automatically connects to the first discovered console on the network if `--ip` is omitted, or connects to `--ip <IP>` when specified.
- **NAND Flash Management**: Fetch console info (`CPU Key`, `DVD Key`, `fuses`, kernel version), list bad blocks, dump NAND, read/write/erase raw blocks.
- **Memory & Hypervisor Operations**: Peek/poke physical memory and HV memory, dump 1BL ROM, dump full HV.
- **File Management**: Send and receive files to/from storage devices (`Hdd:`, `Usb:`, etc.), mount/unmount paths, create directories.
- **Power Management**: Reboot, SMC reset, and shutdown console remotely.
- **JSON Formatting**: Add `--json` to any command for JSON output.

## Building

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

## Usage Examples

```bash
# Auto-discover Xbox 360 consoles on the local network
./updclient discover

# Get console NAND info, CPU key, DVD key (auto-connects or specify --ip)
./updclient --ip 192.168.1.100 info

# Output info in JSON format
./updclient --ip 192.168.1.100 --json info

# Dump full console NAND flash
./updclient --ip 192.168.1.100 nand dump -o my_nand.bin

# Get bad block list
./updclient --ip 192.168.1.100 nand badblocks

# Download a file from the console
./updclient --ip 192.168.1.100 file get "Hdd:\launch.ini" "./launch.ini"

# Reboot console
./updclient --ip 192.168.1.100 power reboot
```
