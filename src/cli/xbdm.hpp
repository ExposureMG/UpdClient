#pragma once

#include "cli/context.hpp"

#include <cstdint>
#include <string>

namespace updclient::cli {

// The XBDM side of the commands that UpdServer and XBDM share. The file, mem,
// power and info groups call these when --target is an xbdm:// endpoint. Console
// paths are "HDD:\dir\file" or "/HDD/dir/file".
Outcome<void> xbdmGet(Context &context, const std::string &remote, const std::string &local);
Outcome<void> xbdmSend(Context &context, const std::string &local, const std::string &remote);
Outcome<void> xbdmMkdir(Context &context, const std::string &remote);
Outcome<void> xbdmInfo(Context &context);
Outcome<void> xbdmPeek(Context &context, uint32_t address, uint32_t length);
Outcome<void> xbdmPoke(Context &context, uint32_t address, uint32_t value);
Outcome<void> xbdmReboot(Context &context, bool cold);
Outcome<void> xbdmShutdown(Context &context);

} // namespace updclient::cli
