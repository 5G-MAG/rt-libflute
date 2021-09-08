// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the “License”).  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an “AS IS” BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
// 
// See the License for the specific language governing permissions and limitations
// under the License.
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
    {"port", 'p', "PORT", 0, "Target port (default: 40085)", 0},
    {"mtu", 't', "BYTES", 0, "Path MTU to size ALC packets for (default: 1500)", 0},
    {"rate-limit", 'r', "KBPS", 0, "Transmit rate limit (kbps), 0 = no limit, default: 1000 (1 Mbps)", 0},
    {"ipsec-key", 'k', "KEY", 0, "To enable IPSec/ESP encryption of packets, provide a hex-encoded AES key here", 0},
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
  bool enable_ipsec = false;
  const char *aes_key = {};
  unsigned short mcast_port = 40085;
  unsigned short mtu = 1500;
  uint32_t rate_limit = 1000;
  unsigned log_level = 2;        /**< log level */
  char **files;
};

/**
 * Parses the command line options into the arguments struct.
 */
static auto parse_opt(int key, char *arg, struct argp_state *state) -> error_t {
  auto arguments = static_cast<struct ft_arguments *>(state->input);
  switch (key) {
    case 'm':
      arguments->mcast_target = arg;
      break;
    case 'k':
      arguments->aes_key = arg;
      arguments->enable_ipsec = true;
      break;
    case 'p':
      arguments->mcast_port = static_cast<unsigned short>(strtoul(arg, nullptr, 10));
      break;
    case 't':
      arguments->mtu = static_cast<unsigned short>(strtoul(arg, nullptr, 10));
      break;
    case 'r':
      arguments->rate_limit = static_cast<uint32_t>(strtoul(arg, nullptr, 10));
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
  spdlog::info("FLUTE transmitter demo starting up");


  // We're responsible for buffer management, so create a vector of structs that
  // are going to hold the data buffers
  struct FsFile {
    std::string location;
    char* buffer;
    size_t len;
    uint32_t toi;
  };
  std::vector<FsFile> files;

  // read the file contents into the buffers
  for (int j = 0; arguments.files[j]; j++) {
    std::string location = arguments.files[j];
    std::ifstream file(arguments.files[j], std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    char* buffer = (char*)malloc(size);
    file.read(buffer, size);
    files.push_back(FsFile{ arguments.files[j], buffer, (size_t)size});
  }

  // Create a Boost io_service
  boost::asio::io_service io;

  // Construct the transmitter class
  LibFlute::Transmitter transmitter(
      arguments.mcast_target,
      arguments.mcast_port,
      0,
      arguments.mtu,
      arguments.rate_limit,
      io);

  // Configure IPSEC ESP, if enabled
  if (arguments.enable_ipsec) 
  {
    transmitter.enable_ipsec(1, arguments.aes_key);
  }

  // Register a completion callback
  transmitter.register_completion_callback(
      [&files](uint32_t toi) {
        for (auto& file : files) {
          if (file.toi == toi) { 
            spdlog::info("{} (TOI {}) has been transmitted",
              file.location, file.toi);
            // could free() the buffer here
          }
        }
      });

  // Queue all the files 
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

  // Start the io_service, and thus sending data
  io.run();

exit:
  return 0;
}
