// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//               2025 British Broadcasting Corporation (David Waring <david.waring2@bbc.co.uk>)
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
#include <argp.h>

#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

#include <libconfig.h++>
#include <boost/asio.hpp>
#include <openssl/sha.h>

#include "spdlog/async.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/syslog_sink.h"

#include "Version.h"
#include "../utils/base64.h"
#include "Transmitter.h"


using libconfig::Config;
using libconfig::FileIOException;
using libconfig::ParseException;

using namespace std::literals::chrono_literals;

static void print_version(FILE *stream, struct argp_state *state);
void (*argp_program_version_hook)(FILE *, struct argp_state *) = print_version;
const char *argp_program_bug_address = "Austrian Broadcasting Services <obeca@ors.at>";
static char doc[] = "FLUTE/ALC transmitter demo";  // NOLINT

static struct argp_option options[] = {  // NOLINT
    {"target", 'm', "IP", 0, "Target multicast address (default: 238.1.1.95)", 0},
    {"port", 'p', "PORT", 0, "Target port (default: 40085)", 0},
    {"mtu", 't', "BYTES", 0, "Path MTU to size ALC packets for (default: 1500)", 0},
    {"rate-limit", 'r', "KBPS", 0, "Transmit rate limit (kbps), 0 = no limit, default: 1000 (1 Mbps)", 0},
    {"deactivate-mode", 'd', "MODE", 0, "Deactivation mode for --deactivate-after or Ctrl-C: immediate|drain (default: immediate)", 0},
    {"deactivate-after", 'a', "MS", 0, "Request transmitter deactivation after this many milliseconds (default: disabled)", 0},
    {"ipsec-key", 'k', "KEY", 0, "To enable IPSec/ESP encryption of packets, provide a hex-encoded AES key here", 0},
    {"log-level", 'l', "LEVEL", 0,
     "Log verbosity: 0 = trace, 1 = debug, 2 = info, 3 = warn, 4 = error, 5 = "
     "critical, 6 = none (default: 2)",
     0},
    {"gzip", 'g', nullptr, 0, "Use gzip to compress the contents, implies -n option", 0},
    {"tsi", 'T', "ID", 0, "The TSI to use for the FLUTE session (default: 16)", 0},
    {"new-api", 'n', nullptr, 0, "Use the new FileDescription API", 0},
    {"retransmit", 'R', "COUNT", 0, "Number of times to repeatedly transmit a file, implies -n option (default: 1)", 0},
    {"etags", 'e', nullptr, 0, "Enable generation of ETag values for each file, implies -n option (default: no ETags)", 0},
    {nullptr, 0, nullptr, 0, nullptr, 0}};

/**
 * Holds all options passed on the command line
 */
struct ft_arguments {
  enum class DeactivateMode : std::uint8_t {
    Immediate,
    Drain,
  };

  const char *mcast_target = {};
  bool enable_ipsec = false;
  bool use_gzip = false;
  bool new_api = false;
  bool gen_etags = false;
  const char *aes_key = {};
  unsigned short mcast_port = 40085;
  unsigned short mtu = 1500;
  uint32_t rate_limit = 1000;
  uint64_t tsi = 16;
  size_t retransmit_count = 1;
  DeactivateMode deactivate_mode = DeactivateMode::Immediate;
  uint32_t deactivate_after_ms = 0;
  unsigned log_level = 2;        /**< log level */
  char **files;
};

struct deactivation_demo_state {
  bool deactivation_requested = false;
};

static auto request_deactivation(
    LibFlute::Transmitter& transmitter,
    boost::asio::io_context& io,
    const ft_arguments& arguments,
    const std::shared_ptr<deactivation_demo_state>& state,
    bool force_immediate = false) -> void {
  if (state->deactivation_requested && !force_immediate) {
    return;
  }

  const bool drain = !force_immediate && arguments.deactivate_mode == ft_arguments::DeactivateMode::Drain;
  const auto queued_files = transmitter.number_of_files();
  if (drain) {
    spdlog::info("Deactivation requested, draining {} queued transmissions before stopping", queued_files);
  } else {
    spdlog::info("Deactivating immediately, {} queued transmissions may remain unsent", queued_files);
  }

  state->deactivation_requested = true;
  transmitter.deactivate(drain);
  if (!drain || queued_files == 0) {
    io.stop();
  }
}

static auto configure_deactivation_demo(
    const ft_arguments& arguments,
    boost::asio::io_context& io,
    LibFlute::Transmitter& transmitter,
    const std::shared_ptr<deactivation_demo_state>& state) -> void {
  if (arguments.deactivate_after_ms > 0) {
    auto timer = std::make_shared<boost::asio::deadline_timer>(io);
    timer->expires_from_now(boost::posix_time::milliseconds(arguments.deactivate_after_ms));
    timer->async_wait(
        [timer, &arguments, &io, &transmitter, state](const boost::system::error_code& error) {
          if (error == boost::asio::error::operation_aborted) {
            return;
          }
          request_deactivation(transmitter, io, arguments, state);
        });
  }

  auto signals = std::make_shared<boost::asio::signal_set>(io, SIGINT, SIGTERM);
  auto handler = std::make_shared<std::function<void(const boost::system::error_code&, int)>>();
  *handler = [signals, handler, &arguments, &io, &transmitter, state](
                 const boost::system::error_code& error,
                 int signal_number) {
    if (error == boost::asio::error::operation_aborted) {
      return;
    }

    if (state->deactivation_requested) {
      spdlog::warn("Received signal {}, forcing immediate shutdown", signal_number);
      request_deactivation(transmitter, io, arguments, state, true);
      return;
    }

    spdlog::info("Received signal {}, requesting transmitter deactivation", signal_number);
    request_deactivation(transmitter, io, arguments, state);
    signals->async_wait(*handler);
  };
  signals->async_wait(*handler);
}

/**
 * Parses the command line options into the arguments struct.
 */
static auto parse_opt(int key, char *arg, struct argp_state *state) -> error_t {
  auto arguments = static_cast<struct ft_arguments *>(state->input);
  switch (key) {
    case 'e':
      arguments->gen_etags = true;
      arguments->new_api = true;
      break;
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
    case 'd': {
      const std::string mode(arg);
      if (mode == "immediate") {
        arguments->deactivate_mode = ft_arguments::DeactivateMode::Immediate;
      } else if (mode == "drain") {
        arguments->deactivate_mode = ft_arguments::DeactivateMode::Drain;
      } else {
        argp_error(state, "Invalid deactivate mode '%s'. Use immediate or drain.", arg);
      }
      break;
    }
    case 'a':
      arguments->deactivate_after_ms = static_cast<uint32_t>(strtoul(arg, nullptr, 10));
      break;
    case 'l':
      arguments->log_level = static_cast<unsigned>(strtoul(arg, nullptr, 10));
      break;
    case 'g':
      arguments->use_gzip = true;
      arguments->new_api = true;
      break;
    case 'T':
      arguments->tsi = static_cast<uint64_t>(strtoul(arg, nullptr, 10));
      break;
    case 'n':
      arguments->new_api = true;
      break;
    case 'R':
      arguments->retransmit_count = static_cast<size_t>(strtoul(arg, nullptr, 10));
      arguments->new_api = true;
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

static void send_with_new_api(struct ft_arguments &arguments)
{
  struct fileEntry {
    fileEntry(LibFlute::Transmitter::FileDescription *fd, size_t init_count = 0) :file(fd), transmitted_count(init_count) {};

    std::shared_ptr<LibFlute::Transmitter::FileDescription> file;
    size_t transmitted_count;
  };

  std::list<fileEntry> files;

  for (int j = 0; arguments.files[j]; j++) {
    auto fd = new LibFlute::Transmitter::FileDescription(arguments.files[j], arguments.files[j]);
    fd->set_content_type("application/octet-stream");
    fd->set_expiry_time(std::chrono::system_clock::now() + 60s);
    if (arguments.use_gzip) {
      fd->set_compression(LibFlute::Transmitter::FileDescription::COMPRESSION_GZIP);
    }
    if (arguments.gen_etags) {
      std::array<unsigned char, SHA_DIGEST_LENGTH> digest;
      SHA1(reinterpret_cast<const unsigned char*>(fd->data()), fd->data_length(), digest.data());
      fd->set_etag(base64_encode(digest.data(), SHA_DIGEST_LENGTH));
    }
    files.emplace_back(fd);
  }

  // Create a Boost io_context
  boost::asio::io_context io;
  auto deactivation_state = std::make_shared<deactivation_demo_state>();

  // Construct the transmitter class
  LibFlute::Transmitter transmitter(
        arguments.mcast_target,
        (short)arguments.mcast_port,
        arguments.tsi,
        arguments.mtu,
        arguments.rate_limit,
        io, std::nullopt, LibFlute::FileDeliveryTable::FDT_NS_DRAFT_2005);

  // Configure IPSEC ESP, if enabled
  if (arguments.enable_ipsec)
  {
    transmitter.enable_ipsec(1, arguments.aes_key);
  }

  configure_deactivation_demo(arguments, io, transmitter, deactivation_state);

  // Register a completion callback
  transmitter.register_completion_callback(
        [&files, &arguments, &transmitter, &io, deactivation_state](uint32_t toi) -> void {
          for (auto& f : files) {
            if (f.file->toi() == toi) {
              spdlog::info("{} (TOI {}) has been transmitted", f.file->file_entry().content_location, f.file->toi());
              f.transmitted_count++;
              if (!deactivation_state->deactivation_requested && f.transmitted_count < arguments.retransmit_count) {
                transmitter.send(f.file);
              }
            }
          }

          if (deactivation_state->deactivation_requested && transmitter.number_of_files() == 0) {
            spdlog::info("Queued transmissions drained, stopping transmitter demo");
            io.stop();
          }
        });

  // Queue all the files
  for (const auto& file : files) {
    auto toi = transmitter.send( file.file );
    const auto &file_entry = file.file->file_entry();
    spdlog::info("Queued {} ({} bytes ({} bytes transmitted)) for transmission, TOI is {}",
          file_entry.content_location, file_entry.content_length, file_entry.fec_oti.transfer_length, toi);
  }

  // Start the io_context, and thus sending data
  io.run();
}

static void send_with_old_api(struct ft_arguments &arguments)
{
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
    const std::string &location = arguments.files[j];
    std::ifstream file(location, std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    char* buffer = (char*)malloc(size);
    file.read(buffer, size);
    files.push_back(FsFile{ location, buffer, (size_t)size});
  }

  // Create a Boost io_context
  boost::asio::io_context io;
  auto deactivation_state = std::make_shared<deactivation_demo_state>();

  // Construct the transmitter class
  LibFlute::Transmitter transmitter(
        arguments.mcast_target,
        (short)arguments.mcast_port,
        arguments.tsi,
        arguments.mtu,
        arguments.rate_limit,
        io, std::nullopt, LibFlute::FileDeliveryTable::FDT_NS_DRAFT_2005);

  // Configure IPSEC ESP, if enabled
  if (arguments.enable_ipsec)
  {
    transmitter.enable_ipsec(1, arguments.aes_key);
  }

  configure_deactivation_demo(arguments, io, transmitter, deactivation_state);

  // Register a completion callback
  transmitter.register_completion_callback(
        [&files, &transmitter, &io, deactivation_state](uint32_t toi) -> void {
          for (auto& file : files) {
            if (file.toi == toi) {
              spdlog::info("{} (TOI {}) has been transmitted", file.location, file.toi);
              // could free() the buffer here
            }
          }

          if (deactivation_state->deactivation_requested && transmitter.number_of_files() == 0) {
            spdlog::info("Queued transmissions drained, stopping transmitter demo");
            io.stop();
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

  // Start the io_context, and thus sending data
  io.run();
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
    if (arguments.new_api) {
      send_with_new_api(arguments);
    } else {
      send_with_old_api(arguments);
    }
  } catch (std::exception ex ) {
    spdlog::error("Exiting on unhandled exception: %s", ex.what());
  }

exit:
  return 0;
}
