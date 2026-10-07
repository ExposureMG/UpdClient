#pragma once

#include <core/error.hpp>

#include <CLI/CLI.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace updclient::cli {

// Plain decimal ("4096") or 0x-prefixed hex ("0x1000"); no sign, no suffixes.
Result<uint64_t> parseUnsigned(std::string_view text, uint64_t max = std::numeric_limits<uint64_t>::max());

// A command group ("nand", "mem", ...): an action is required and global options
// may follow it.
CLI::App *addGroup(CLI::App &parent, const std::string &name, const std::string &description);

// Adds an option or positional taking a number as above. The help text states the
// accepted notation. Invalid values are reported as usage errors.
CLI::Option *addNumberOption(CLI::App *app, const std::string &name, const std::string &description,
                             uint64_t max, std::function<void(uint64_t)> assign);

template <std::unsigned_integral T>
CLI::Option *addNumber(CLI::App *app, const std::string &name, T &destination,
                       const std::string &description) {
  return addNumberOption(app, name, description, std::numeric_limits<T>::max(),
                         [&destination](uint64_t value) { destination = static_cast<T>(value); });
}

template <std::unsigned_integral T>
CLI::Option *addNumber(CLI::App *app, const std::string &name, std::optional<T> &destination,
                       const std::string &description) {
  return addNumberOption(app, name, description, std::numeric_limits<T>::max(),
                         [&destination](uint64_t value) { destination = static_cast<T>(value); });
}

} // namespace updclient::cli
