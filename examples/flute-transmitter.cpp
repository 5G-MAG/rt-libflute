// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
// 
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <cstdio>
#include <iostream>
#include <argp.h>

#include <cstdlib>

#include <fstream>
#include <string>
#include <filesystem>
#include <libconfig.h++>
#include <boost/asio.hpp>

#include "spdlog/async.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/syslog_sink.h"

#include "Transmitter.h"


using libconfig::Config;
using libconfig::FileIOException;
using libconfig::ParseException;

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;

static void print_version(FILE *stream, struct argp_state *state);
void (*argp_program_version_hook)(FILE *, struct argp_state *) = print_version;
const char *argp_program_bug_address = "Austrian Broadcasting Services <obeca@ors.at>";
static char doc[] = "FLUTE/ALC transmitter demo";  // NOLINT

static struct argp_option options[] = {  // NOLINT
    {"target", 'm', "IP", 0, "Target multicast address (default: 238.1.1.95)", 0},
    {"port", 'p', "IP", 0, "Target port (default: 40085)", 0},
    {"mtu", 't', "IP", 0, "Path MTU to size ALC packets for (default: 1500)", 0},
    {"log-level", 'l', "LEVEL", 0,
     "Log verbosity: 0 = trace, 1 = debug, 2 = info, 3 = warn, 4 = error, 5 = "
     "critical, 6 = none. Default: 2.",
     0},
    {nullptr, 0, nullptr, 0, nullptr, 0}};

/**
 * Holds all options passed on the command line
 */
struct ft_arguments {
  const char *mcast_target = {};
  unsigned short mcast_port = 40085;
  unsigned short mtu = 1500;
  unsigned log_level = 2;        /**< log level */
  char **files;
};

/**
 * Parses the command line options into the arguments struct.
 */
static auto parse_opt(int key, char *arg, struct argp_state *state) -> error_t {
  auto arguments = static_cast<struct ft_arguments *>(state->input);
  switch (key) {
    case 'c':
      arguments->mcast_target = arg;
      break;
    case 'p':
      arguments->mcast_port = static_cast<unsigned short>(strtoul(arg, nullptr, 10));
      break;
    case 't':
      arguments->mtu = static_cast<unsigned short>(strtoul(arg, nullptr, 10));
      break;
    case 'l':
      arguments->log_level = static_cast<unsigned>(strtoul(arg, nullptr, 10));
      break;
    case ARGP_KEY_NO_ARGS:
      argp_usage (state);
    case ARGP_KEY_ARG:
      arguments->files = &state->argv[state->next-1];
      state->next = state->argc;
      break;
    default:
      return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

static char args_doc[] = "[FILE...]";
static struct argp argp = {options, parse_opt, args_doc, doc,
                           nullptr, nullptr,   nullptr};

/**
 * Print the program version in MAJOR.MINOR.PATCH format.
 */
void print_version(FILE *stream, struct argp_state * /*state*/) {
  fprintf(stream, "1.0.0\n");
}



/**
 *  Main entry point for the program.
 *  
 * @param argc  Command line agument count
 * @param argv  Command line arguments
 * @return 0 on clean exit, -1 on failure
 */
auto main(int argc, char **argv) -> int {
  struct ft_arguments arguments;
  /* Default values */
  arguments.mcast_target = "238.1.1.95";

  argp_parse(&argp, argc, argv, 0, nullptr, &arguments);

  // Set up logging
  std::string ident = "flute-transmitter";
  auto syslog_logger = spdlog::syslog_logger_mt("syslog", ident, LOG_PID | LOG_PERROR | LOG_CONS );

  spdlog::set_level(
      static_cast<spdlog::level::level_enum>(arguments.log_level));
  spdlog::set_pattern("[%H:%M:%S.%f %z] [%^%l%$] [thr %t] %v");

  spdlog::set_default_logger(syslog_logger);
  spdlog::info("FLTUE transmitter demo starting up");


  struct FsFile {
    std::string location;
    char* buffer;
    size_t len;
    uint32_t toi;
  };
  std::vector<FsFile> files;

  // read the file contents into buffers
  for (int j = 0; arguments.files[j]; j++) {
    std::string location = arguments.files[j];
    std::ifstream file(arguments.files[j], std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    char* buffer = (char*)malloc(size);
    file.read(buffer, size);
    files.push_back(FsFile{ arguments.files[j], buffer, (size_t)size});
  }

  boost::asio::io_service io;
  LibFlute::Transmitter transmitter(
      arguments.mcast_target,
      arguments.mcast_port,
      0,
      arguments.mtu,
      io);

  transmitter.register_completion_callback(
      [&files](uint32_t toi) {
        for (auto& file : files) {
          if (file.toi == toi) { 
            spdlog::info("{} (TOI {}) has been transmitted",
              file.location, file.toi);
            free(file.buffer);
          }
        }
      });

  for (auto& file : files) {
    file.toi = transmitter.send( file.location,
        "application/octet-stream",
        transmitter.seconds_since_epoch() + 60, // 1 minute from now
        file.buffer,
        file.len
        );
    spdlog::info("Queued {} ({} bytes) for transmission, TOI is {}",
        file.location, file.len, file.toi);
  }

  io.run();

exit:
  return 0;
}
