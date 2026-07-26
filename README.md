# UpdClient

Modern C++23 CMake command-line client for **UpdServer** (xeBuild / DashLaunch update server on Xbox 360).

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
