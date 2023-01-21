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
#include <sys/mman.h>
#include <sys/stat.h>
#include <fstream>
#include <string>
#include <filesystem>
#include <libconfig.h++>
#include <boost/asio.hpp>

#include "spdlog/async.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/syslog_sink.h"

#include "Version.h"
#include "Transmitter.h"
#include "flute_types.h"


using libconfig::Config;
using libconfig::FileIOException;
using libconfig::ParseException;

static void print_version(FILE *stream, struct argp_state *state);
void (*argp_program_version_hook)(FILE *, struct argp_state *) = print_version;
const char *argp_program_bug_address = "Austrian Broadcasting Services <obeca@ors.at>";
static char doc[] = "FLUTE/ALC transmitter demo";  // NOLINT

static struct argp_option options[] = {  // NOLINT
    {"target", 'm', "IP", 0, "Target multicast address (default: 238.1.1.95)", 0},
    {"fec", 'f', "FEC Scheme", 0, "Choose a scheme for Forward Error Correction. Compact No Code = 0, Raptor = 1 (default is 0)", 0},
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
  unsigned fec = 0;        /**< log level */
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
    case 'f':
      arguments->fec = static_cast<unsigned>(strtoul(arg, nullptr, 10));
      if ( (arguments->fec | 1) != 1 ) {
        spdlog::error("Invalid FEC scheme ! Please pick either 0 (Compact No Code) or 1 (Raptor)");
        return ARGP_ERR_UNKNOWN;
      }
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

static char args_doc[] = "[FILE...]"; //NOLINT
static struct argp argp = {options, parse_opt, args_doc, doc,
                           nullptr, nullptr,   nullptr};

/**
 * Print the program version in MAJOR.MINOR.PATCH format.
 */
void print_version(FILE *stream, struct argp_state * /*state*/) {
  fprintf(stream, "%s.%s.%s\n", std::to_string(VERSION_MAJOR).c_str(),
          std::to_string(VERSION_MINOR).c_str(),
          std::to_string(VERSION_PATCH).c_str());
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

  try {
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
      struct stat sb;
      int fd;
      fd = open(arguments.files[j], O_RDONLY);
      if (fd == -1) {
        spdlog::error("Couldnt open file {}",arguments.files[j]);
        continue;
      } 
      if (fstat(fd, &sb) == -1){ // To obtain file size
        spdlog::error("fstat() call for file {} failed",arguments.files[j]);
        close(fd);
        continue;
      }
      if (sb.st_size <= 0) {
        close(fd);
        continue;
      }
      char* buffer = (char*) mmap(nullptr, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      close(fd);
      if ( (long) buffer <= 0) {
        spdlog::error("mmap() failed for file {}",arguments.files[j]);
        continue;
      }
      files.push_back(FsFile{ arguments.files[j], buffer, (size_t) sb.st_size});
    }

    // Create a Boost io_service
    boost::asio::io_service io;

    // Construct the transmitter class
    LibFlute::Transmitter transmitter(
        arguments.mcast_target,
        (short)arguments.mcast_port,
        16,
        arguments.mtu,
        arguments.rate_limit,
        LibFlute::FecScheme(arguments.fec),
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
            spdlog::info("{} (TOI {}) has been transmitted", file.location,file.toi);
            munmap(file.buffer,file.len);
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
      if (file.toi > 0) {
        spdlog::info("Queued {} ({} bytes) for transmission, TOI is {}",
          file.location, file.len, file.toi);
      }
    }

    // Start the io_service, and thus sending data
    io.run();
  } catch (std::exception ex ) {
    spdlog::error("Exiting on unhandled exception: %s", ex.what());
  }

exit:
  return 0;
}
