#include "cli/args.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace updclient::cli {

Result<uint64_t> parseUnsigned(std::string_view text, uint64_t max) {
  int base = 10;
  std::string_view digits = text;
  if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
    base = 16;
    digits.remove_prefix(2);
  }
  const auto isDigit = [base](char c) {
    if (c >= '0' && c <= '9') return true;
    return base == 16 && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
  };
  if (digits.empty() || !std::all_of(digits.begin(), digits.end(), isDigit)) {
    return fail(ErrorCode::InvalidArgument,
                "'" + std::string(text) + "' is not a number (use decimal or 0x-prefixed hex)");
  }

  uint64_t value = 0;
  const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
  if (ec == std::errc::result_out_of_range || ptr != digits.data() + digits.size() || value > max) {
    return fail(ErrorCode::InvalidArgument,
                "'" + std::string(text) + "' is out of range (maximum " + std::to_string(max) + ")");
  }
  if (ec != std::errc{}) {
    return fail(ErrorCode::InvalidArgument, "'" + std::string(text) + "' is not a number");
  }
  return value;
}

CLI::App *addGroup(CLI::App &parent, const std::string &name, const std::string &description) {
  CLI::App *group = parent.add_subcommand(name, description);
  group->require_subcommand(1);
  group->fallthrough();
  return group;
}

CLI::Option *addNumberOption(CLI::App *app, const std::string &name, const std::string &description,
                             uint64_t max, std::function<void(uint64_t)> assign) {
  const std::string optionName = name.substr(0, name.find(','));
  CLI::Option *option = app->add_option_function<std::string>(
      name,
      [optionName, max, assign = std::move(assign)](const std::string &text) {
        auto value = parseUnsigned(text, max);
        if (!value) throw CLI::ValidationError(optionName, value.error().message);
        assign(*value);
      },
      description + "; decimal or 0x-prefixed hex");
  option->type_name("NUMBER");
  return option;
}

} // namespace updclient::cli
