// libflute - FLUTE/ALC library
//
// The flute-receiver example, run as the program it is.
//
// 5G-MAG/rt-libflute#27 reported three defects in where the receiver writes what it receives: a
// forged Content-Location could write outside the intended directory, a Content-Location URI was
// used as a file path unparsed, and a file that failed to open crashed the program. The fix lives in
// a function local to the example, so these tests drive the real binary: an in-process Transmitter
// sends one object with a chosen Content-Location, and the test inspects the filesystem.

#include <gtest/gtest.h>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/asio.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "Transmitter.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr uint64_t kTsi = 27;

fs::path make_temp_dir() {
  std::string tmpl = (fs::temp_directory_path() / "flute-example-XXXXXX").string();
  char* made = mkdtemp(tmpl.data());
  if (!made) throw std::runtime_error("mkdtemp failed");
  return fs::path(made);
}

/* The receiver, started in `cwd`, listening on the given group and port. */
class ReceiverProcess {
 public:
  ReceiverProcess(const fs::path& cwd, const std::string& group, unsigned short port,
                  const std::string& output_path, const fs::path& log) {
    _pid = fork();
    if (_pid == 0) {
      if (chdir(cwd.c_str()) != 0) _exit(126);
      FILE* out = freopen(log.c_str(), "w", stdout);
      FILE* err = freopen(log.c_str(), "a", stderr);
      (void)out;
      (void)err;
      const std::string port_s = std::to_string(port), tsi_s = std::to_string(kTsi);
      std::vector<const char*> argv = {FLUTE_RECEIVER_BIN, "-m", group.c_str(), "-p", port_s.c_str(),
                                       "-T", tsi_s.c_str(), "-l", "2"};
      if (!output_path.empty()) {
        argv.push_back("-o");
        argv.push_back(output_path.c_str());
      }
      argv.push_back(nullptr);
      execv(FLUTE_RECEIVER_BIN, const_cast<char* const*>(argv.data()));
      _exit(127);
    }
    std::this_thread::sleep_for(700ms);  // let it join the group before anything is sent
  }
  ~ReceiverProcess() {
    if (_pid > 0 && still_running()) {
      kill(_pid, SIGTERM);
      waitpid(_pid, nullptr, 0);
    }
  }
  bool still_running() {
    if (_exited) return false;
    int status = 0;
    if (waitpid(_pid, &status, WNOHANG) == _pid) {
      _exited = true;
      return false;
    }
    return true;
  }

 private:
  pid_t _pid = -1;
  bool _exited = false;
};

/* Sends one object and returns once the Transmitter reports it fully sent. */
void send_object(const std::string& group, unsigned short port, const std::string& location) {
  boost::asio::io_context io;
  LibFlute::Transmitter tx(group, port, kTsi, /*mtu*/ 1400, /*rate_limit*/ 0, io);
  std::promise<void> sent;
  auto sent_future = sent.get_future();
  tx.register_completion_callback([&sent, &tx, &io](uint32_t) {
    sent.set_value();
    tx.deactivate();
    io.stop();
  });
  const std::vector<char> payload(2000, 'x');
  auto fd = std::make_shared<LibFlute::Transmitter::FileDescription>(location, payload);
  fd->set_content_type("application/octet-stream");
  std::thread io_thread([&io] { io.run(); });
  tx.send(fd);
  const bool done = sent_future.wait_for(5s) == std::future_status::ready;
  io.stop();
  io_thread.join();
  if (!done) throw std::runtime_error("transmitter did not report completion");
}

bool appears(const fs::path& p) {
  for (int i = 0; i < 50; i++) {
    if (fs::exists(p)) return true;
    std::this_thread::sleep_for(100ms);
  }
  return false;
}

}  // namespace

/* Bug 1: with no output directory, a Content-Location that climbs with ".." must not write outside
   the directory the receiver was started in. */
TEST(FluteReceiverExampleTest, AForgedContentLocationCannotEscapeTheWorkingDirectory) {
  const auto root = make_temp_dir();
  const auto work = root / "work";
  fs::create_directories(work);
  ReceiverProcess rx(work, "239.255.27.1", 40271, "", root / "rx.log");
  send_object("239.255.27.1", 40271, "../escaped-27a.bin");

  EXPECT_TRUE(appears(work / "escaped-27a.bin")) << "not written inside the working directory";
  EXPECT_FALSE(fs::exists(root / "escaped-27a.bin")) << "written outside the working directory";
  fs::remove_all(root);
}

/* Bug 2: a Content-Location is a URI. Its path is kept, so a hierarchical location lands in a
   matching subdirectory of the output directory, and scheme, host and query are not part of it. */
TEST(FluteReceiverExampleTest, AContentLocationUriIsReducedToItsPath) {
  const auto root = make_temp_dir();
  const auto out = root / "out";
  fs::create_directories(out);
  ReceiverProcess rx(root, "239.255.27.2", 40272, out.string(), root / "rx.log");
  send_object("239.255.27.2", 40272, "http://example.invalid/media/seg-27b.m4s?token=1");

  EXPECT_TRUE(appears(out / "media" / "seg-27b.m4s")) << "URI path not used under the output directory";
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    EXPECT_EQ(e.path().string().find('?'), std::string::npos) << "query kept in " << e.path();
    EXPECT_EQ(e.path().string().find("example.invalid"), std::string::npos) << "host kept in " << e.path();
  }
  fs::remove_all(root);
}

/* Bug 3: a file that cannot be written must be reported, not crash the receiver. Both the flat case,
   where fopen() fails, and the case with a subdirectory to create first. Skipped as root, for whom a
   read-only directory is still writable. */
TEST(FluteReceiverExampleTest, AnUnwritableOutputDirectoryDoesNotCrashTheReceiver) {
  if (geteuid() == 0) GTEST_SKIP() << "running as root, a read-only directory is still writable";
  const auto root = make_temp_dir();
  const auto out = root / "read-only";
  fs::create_directories(out);
  chmod(out.c_str(), 0555);
  ReceiverProcess rx(root, "239.255.27.3", 40273, out.string(), root / "rx.log");

  send_object("239.255.27.3", 40273, "flat-27c.bin");
  std::this_thread::sleep_for(1s);
  EXPECT_TRUE(rx.still_running()) << "receiver exited when fopen() failed";

  send_object("239.255.27.3", 40273, "sub/nested-27d.bin");
  std::this_thread::sleep_for(1s);
  EXPECT_TRUE(rx.still_running()) << "receiver exited when it could not create a subdirectory";

  EXPECT_FALSE(fs::exists(out / "flat-27c.bin"));
  chmod(out.c_str(), 0755);
  fs::remove_all(root);
}
