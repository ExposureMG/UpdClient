#pragma once

#include <core/error.hpp>
#include <core/export.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace updclient {

UPDCLIENT_API std::string formatHex(std::span<const uint8_t> bytes);

// Requires exactly out.size() * 2 hex digits (either case).
UPDCLIENT_API Result<void> parseHex(std::string_view hex, std::span<uint8_t> out);

} // namespace updclient
