#pragma once

namespace updclient::cli {

// Parses argv, runs the selected command and returns the process exit code.
int run(int argc, char **argv);

} // namespace updclient::cli
