// libflute - FLUTE/ALC library
//
// IPsec ESP configuration, checked against the state the kernel actually holds.
//
// LibFlute::IpSec::enable_esp() installs an XFRM security association and policy over netlink, so
// the only honest check is to read them back. Each test runs in a child process inside its own user
// and network namespace, with root mapped to the caller: the XFRM state is private to that namespace,
// needs no privilege on the host, and disappears with the child. Where the host does not permit
// unprivileged namespaces the tests skip and say so, rather than pass on nothing.

#include <gtest/gtest.h>

#include <dirent.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <functional>
#include <string>

#include "IpSec.h"

namespace {

constexpr int kNamespaceUnavailable = 77;
const std::string kKey = "00112233445566778899aabbccddeeff";

bool write_file(const std::string& path, const std::string& content) {
  std::ofstream f(path);
  f << content;
  return static_cast<bool>(f);
}

struct NamespaceRun {
  bool available;
  std::string output;
};

/* Runs body in a forked child inside fresh user and network namespaces and returns what it wrote.
   The child is single-threaded after fork(), which unshare(CLONE_NEWUSER) requires. */
NamespaceRun in_private_network_namespace(const std::function<std::string()>& body) {
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) return {false, "pipe failed"};
  const uid_t uid = getuid();
  const gid_t gid = getgid();
  const pid_t pid = fork();
  if (pid == 0) {
    close(pipe_fds[0]);
    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) _exit(kNamespaceUnavailable);
    write_file("/proc/self/setgroups", "deny");
    if (!write_file("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1") ||
        !write_file("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1")) {
      _exit(kNamespaceUnavailable);
    }
    std::string out;
    try {
      out = body();
    } catch (const std::exception& e) {
      out = std::string("threw: ") + e.what();
    }
    ssize_t written = write(pipe_fds[1], out.data(), out.size());
    (void)written;
    _exit(0);
  }
  close(pipe_fds[1]);
  std::string output;
  char buf[4096];
  ssize_t n;
  while ((n = read(pipe_fds[0], buf, sizeof buf)) > 0) output.append(buf, static_cast<size_t>(n));
  close(pipe_fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  const bool available = !(WIFEXITED(status) && WEXITSTATUS(status) == kNamespaceUnavailable);
  return {available, output};
}

std::string run(const char* command) {
  std::string out;
  FILE* p = popen(command, "r");
  if (!p) return out;
  char buf[512];
  while (fgets(buf, sizeof buf, p)) out += buf;
  pclose(p);
  return out;
}

size_t open_descriptors() {
  size_t n = 0;
  DIR* d = opendir("/proc/self/fd");
  if (!d) return 0;
  while (readdir(d)) n++;
  closedir(d);
  return n;
}

#define SKIP_UNLESS_AVAILABLE(r) \
  if (!(r).available) GTEST_SKIP() << "unprivileged user and network namespaces are not available here"

}  // namespace

/* 5G-MAG/rt-libflute#96: the association used to carry encryption only, with no packet
   authentication. It must carry an authentication algorithm alongside the cipher. */
TEST(IpSecTest, TheAssociationAuthenticatesAsWellAsEncrypts) {
  auto r = in_private_network_namespace([] {
    LibFlute::IpSec::enable_esp(0x101, "239.1.6.1", 5000, LibFlute::IpSec::Direction::Out, kKey);
    return run("ip xfrm state 2>&1");
  });
  SKIP_UNLESS_AVAILABLE(r);
  EXPECT_NE(r.output.find("proto esp spi 0x00000101"), std::string::npos) << r.output;
  EXPECT_NE(r.output.find("hmac(sha256)"), std::string::npos) << "no authentication algorithm:\n"
                                                              << r.output;
  EXPECT_NE(r.output.find("enc cbc(aes)"), std::string::npos) << r.output;
}

/* 5G-MAG/rt-libflute#85: an IPv6 destination used to be accepted and then expressed as IPv4. Both
   the association and the policy must name the IPv6 address. */
TEST(IpSecTest, AnIpv6DestinationIsInstalledAsIpv6) {
  auto r = in_private_network_namespace([] {
    LibFlute::IpSec::enable_esp(0x102, "ff3e::1234", 5000, LibFlute::IpSec::Direction::Out, kKey);
    return run("ip xfrm state 2>&1; echo ---policy---; ip xfrm policy 2>&1");
  });
  SKIP_UNLESS_AVAILABLE(r);
  const auto split = r.output.find("---policy---");
  ASSERT_NE(split, std::string::npos) << r.output;
  EXPECT_NE(r.output.substr(0, split).find("dst ff3e::1234"), std::string::npos)
      << "association:\n" << r.output;
  EXPECT_NE(r.output.substr(split).find("dst ff3e::1234/128"), std::string::npos)
      << "policy:\n" << r.output;
}

/* The policy selects the session's own traffic, by protocol and destination port, and not every
   datagram addressed to the group. RFC 5775 clause 5.1.1: "The sender IPsec SPD entry MUST be
   configured to process outbound packets to the destination address and UDP port number of the
   applicable ALC session." */
TEST(IpSecTest, ThePolicySelectsTheSessionsUdpPort) {
  auto r = in_private_network_namespace([] {
    LibFlute::IpSec::enable_esp(0x103, "239.1.6.3", 5003, LibFlute::IpSec::Direction::Out, kKey);
    return run("ip xfrm policy 2>&1");
  });
  SKIP_UNLESS_AVAILABLE(r);
  EXPECT_NE(r.output.find("proto udp"), std::string::npos) << r.output;
  EXPECT_NE(r.output.find("dport 5003"), std::string::npos) << r.output;
}

/* 5G-MAG/rt-libflute#83: every call used to leave its netlink socket open. Configuring repeatedly
   must not grow the process's descriptor table. */
TEST(IpSecTest, RepeatedConfigurationLeaksNoDescriptor) {
  auto r = in_private_network_namespace([] {
    const size_t before = open_descriptors();
    for (uint32_t i = 0; i < 20; i++) {
      LibFlute::IpSec::enable_esp(0x200 + i, "239.1.6.4", 5004, LibFlute::IpSec::Direction::Out,
                                  kKey);
    }
    const size_t after = open_descriptors();
    return std::to_string(before) + " " + std::to_string(after);
  });
  SKIP_UNLESS_AVAILABLE(r);
  size_t before = 0, after = 0;
  ASSERT_EQ(sscanf(r.output.c_str(), "%zu %zu", &before, &after), 2) << r.output;
  EXPECT_EQ(after, before) << "descriptors before 20 calls: " << before << ", after: " << after;
}
