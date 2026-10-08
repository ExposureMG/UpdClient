#include "cli/app.hpp"

#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "cli/context.hpp"
#include "cli/version.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <iostream>
#include <memory>
#include <sstream>

namespace updclient::cli {

namespace {

constexpr const char *kFooter =
    "Targets: --target takes a URI such as tcp://192.168.1.5:49, or a bare IP address or host name;\n"
    "--ip and --port are shortcuts for tcp targets. With --target xbdm://192.168.1.5 (port 730) the\n"
    "file, mem, power and info commands speak XBDM, the Xbox debug monitor, instead of UpdServer. With\n"
    "--target jrpc://192.168.1.5 (port 1409) the info and power shutdown commands speak JRPC, the console\n"
    "plugin; the jrpc group takes a bare host as well. UpdServer and xbdm commands auto-discover a console\n"
    "when no target is given; xell and jrpc commands never do.\n"
    "Numbers: decimal (4096) or 0x-prefixed hex (0x1000).\n"
    "Output: results go to stdout, logs to stderr; --json prints exactly one JSON document.\n"
    "Exit codes: 0 ok, 1 runtime or transport error, 2 usage error, 3 discovery found nothing or is unavailable.";

void initLogging() {
  auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  auto logger = std::make_shared<spdlog::logger>("updclient", sink);
  logger->set_pattern("%^%l%$: %v");
  spdlog::set_default_logger(std::move(logger));
}

void addGlobalOptions(CLI::App &app, Context &context) {
  GlobalOptions &options = context.options;
  app.add_option("-t,--target", options.target,
                 "Target URI, e.g. tcp://192.168.1.5:49, or a bare IP address or host name (default: auto-discover for UpdServer)");
  app.add_option("-i,--ip", options.ip, "Target IP address or host name (shortcut for --target)");
  addNumber(&app, "-p,--port", options.port, "TCP port, default 49 (UpdServer), 730 (xbdm) or 1409 (jrpc)");
  addNumber(&app, "--xell-port", options.xellPort, "XeLL HTTPD port for xell commands, default 80");
  addNumber(&app, "--timeout-ms", options.timeoutMs, "Connect and I/O timeout in milliseconds, 0 for none, default 5000");
  addNumber(&app, "--discovery-timeout-ms", options.discoveryTimeoutMs, "How long discovery listens, in milliseconds")
      ->default_str("3000");
  app.add_flag("-j,--json", options.json, "Print results and errors as one JSON document on stdout");
  app.add_flag("-v,--verbose", options.verbose, "Log debug detail to stderr");
  app.add_flag("--yes", options.yes, "Skip the confirmation required by destructive commands");
  app.add_option("--trace", options.trace,
                 "Append every XBDM or JRPC command line and every line received to this file (file data is "
                 "never written, only its size)");
}

// The deepest subcommand that was parsed, so help can describe the command the
// user was in the middle of typing.
CLI::App *deepestParsed(CLI::App *app) {
  for (;;) {
    const auto parsed = app->get_subcommands();
    if (parsed.empty()) return app;
    app = parsed.back();
  }
}

int handleParseError(CLI::App &app, Context &context, const CLI::ParseError &error) {
  context.output.setJson(context.options.json);

  if (error.get_exit_code() == 0) {
    if (error.get_name() == "CallForHelp" || error.get_name() == "CallForAllHelp") {
      std::cout << deepestParsed(&app)->help();
      return kExitOk;
    }
    return app.exit(error);
  }

  CLI::App *where = deepestParsed(&app);
  const bool isRoot = where == &app;
  const std::string label = isRoot ? "updclient" : where->get_name();
  const auto leftover = app.remaining(true);

  std::string message;
  bool showHelp = false;
  if (error.get_name() == "RequiredError" && where->get_require_subcommand_min() > 0 &&
      where->get_subcommands().empty()) {
    showHelp = true;
    if (!leftover.empty()) {
      message = "unknown " + std::string(isRoot ? "command" : "action") + " '" + leftover.front() + "'";
    } else {
      message = "'" + label + "' needs " + (isRoot ? "a command" : "an action");
    }
  } else {
    message = error.what();
  }

  spdlog::error("{}", message);
  if (showHelp) {
    std::cerr << "\n" << where->help();
  } else {
    std::cerr << "Run '" << label << " --help' for usage.\n";
  }
  context.output.error("Usage", message);
  return kExitUsage;
}

} // namespace

int run(int argc, char **argv) {
  initLogging();
  registerBuiltins();
  xbdm::registerXbdmScheme();
  jrpc::registerJrpcScheme();

  Context context;
  CLI::App app{std::string("UpdClient - client for the Xbox 360 UpdServer, XeLL, XBDM and JRPC network services"),
               "updclient"};
  app.set_version_flag("--version", std::string("updclient ") + kVersion);
  app.require_subcommand(1);
  app.fallthrough();
  app.set_help_flag("-h,--help", "Show help");
  addGlobalOptions(app, context);
  app.parse_complete_callback([&context] { context.applyGlobals(); });

  registerDiscoverCommand(app, context);
  registerInfoCommands(app, context);
  registerPowerCommands(app, context);
  registerNandCommands(app, context);
  registerMemCommands(app, context);
  registerFileCommands(app, context);
  registerXellCommands(app, context);
  registerXbdmCommands(app, context);
  registerJrpcCommands(app, context);
  app.footer(kFooter);

  try {
    argv = app.ensure_utf8(argv);
    app.parse(argc, argv);
  } catch (const CLI::ParseError &error) {
    return handleParseError(app, context, error);
  } catch (const std::exception &error) {
    context.finish(failWith(kExitRuntime, "Unknown", error.what()));
  }
  return context.exitCode;
}

} // namespace updclient::cli
