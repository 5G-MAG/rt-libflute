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

#include "Version.h"
#include "Receiver.h"
#include "File.h"
#include "flute_types.h"


using libconfig::Config;
using libconfig::FileIOException;
using libconfig::ParseException;

static void print_version(FILE *stream, struct argp_state *state);
void (*argp_program_version_hook)(FILE *, struct argp_state *) = print_version;
const char *argp_program_bug_address = "Austrian Broadcasting Services <obeca@ors.at>";
static char doc[] = "FLUTE/ALC receiver demo";  // NOLINT

static struct argp_option options[] = {  // NOLINT
    {"interface", 'i', "IF", 0, "IP address of the interface to bind flute receivers to (default: 0.0.0.0)", 0},
    {"target", 'm', "IP", 0, "Multicast address to receive on (default: 238.1.1.95)", 0},
    {"port", 'p', "PORT", 0, "Multicast port (default: 40085)", 0},
    {"ipsec-key", 'k', "KEY", 0, "To enable IPSec/ESP decryption of packets, provide a hex-encoded AES key here", 0},
    {"log-level", 'l', "LEVEL", 0,
     "Log verbosity: 0 = trace, 1 = debug, 2 = info, 3 = warn, 4 = error, 5 = "
     "critical, 6 = none. Default: 2.",
     0},
    {"download-dir", 'd', "Download directory", 0 , "Directory in which to store downloaded files, defaults to the current directory otherwise", 0},
    {"num-files", 'n', "Stop Receiving after n files", 0, "Stop the reception after n files have been received (default is to never stop)", 0},
    {nullptr, 0, nullptr, 0, nullptr, 0}};

/**
 * Holds all options passed on the command line
 */
struct ft_arguments {
  const char *flute_interface = {};  /**< file path of the config file. */
  const char *mcast_target = {};
  bool enable_ipsec = false;
  const char *aes_key = {};
  unsigned short mcast_port = 40085;
  unsigned log_level = 2;        /**< log level */
  char *download_dir = nullptr;
  unsigned nfiles = 0;        /**< log level */
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
    case 'i':
      arguments->flute_interface = arg;
      break;
    case 'k':
      arguments->aes_key = arg;
      arguments->enable_ipsec = true;
      break;
    case 'p':
      arguments->mcast_port = static_cast<unsigned short>(strtoul(arg, nullptr, 10));
      break;
    case 'l':
      arguments->log_level = static_cast<unsigned>(strtoul(arg, nullptr, 10));
      break;
    case 'd':
      arguments->download_dir = arg;
      break;
    case 'n':
      arguments->nfiles = static_cast<unsigned>(strtoul(arg, nullptr, 10));
      break;
    default:
      return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

static struct argp argp = {options, parse_opt, nullptr, doc,
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
  arguments.flute_interface= "0.0.0.0";

  // Parse the arguments
  argp_parse(&argp, argc, argv, 0, nullptr, &arguments);

  // Set up logging
  std::string ident = "flute-receiver";
  auto syslog_logger = spdlog::syslog_logger_mt("syslog", ident, LOG_PID | LOG_PERROR | LOG_CONS );

  spdlog::set_level(
      static_cast<spdlog::level::level_enum>(arguments.log_level));
  spdlog::set_pattern("[%H:%M:%S.%f %z] [%^%l%$] [thr %t] %v");

  spdlog::set_default_logger(syslog_logger);
  spdlog::info("FLUTE receiver demo starting up");

  try {
    // Create a Boost io_service
    boost::asio::io_service io;

    // Create the receiver
    LibFlute::Receiver receiver(
        arguments.flute_interface,
        arguments.mcast_target,
        (short)arguments.mcast_port,
        16,
        io);

    // Configure IPSEC, if enabled
    if (arguments.enable_ipsec) 
    {
      receiver.enable_ipsec(1, arguments.aes_key);
    }

    receiver.register_completion_callback(
        [&](std::shared_ptr<LibFlute::File> file) { //NOLINT
        spdlog::info("{} (TOI {}) has been received",
            file->meta().content_location, file->meta().toi);
        char *buf = (char*) calloc(256,1);
        char *fname = (char*) strrchr(file->meta().content_location.c_str(),'/');
        if(!fname){
          fname = (char*) file->meta().content_location.c_str();
        } else {
          fname++;
        }
        if (arguments.download_dir) {
          snprintf(buf,256,"%s/%s",arguments.download_dir, fname);
        } else {
          snprintf(buf,256,"flute_download_%d-%s",file->meta().toi, fname);
        }
        FILE* fd = fopen(buf, "wb");
        if (fd) {
          fwrite(file->buffer(), 1, file->length(), fd);
          fclose(fd);
        } else {
          spdlog::error("Error opening file {} to store received object",buf);
        }
        free(buf);
        if (file->meta().toi == arguments.nfiles) {
          spdlog::warn("{} file(s) received. Stopping reception",arguments.nfiles);
          receiver.stop();
        }
        });

    // Start the IO service
    io.run();
  } catch (std::exception ex ) {
    spdlog::error("Exiting on unhandled exception: {}", ex.what());
  }

exit:
  return 0;
}
