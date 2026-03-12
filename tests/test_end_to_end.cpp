// libflute - FLUTE/ALC library
//
// Copyright (C) 2026 Fraunhofer FOKUS
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the "License").  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
//
// See the License for the specific language governing permissions and limitations
// under the License.
//

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Receiver.h"
#include "Transmitter.h"

namespace {

using namespace std::chrono_literals;

constexpr short kPort = 18091;
constexpr short kTunnelPort = 18092;
constexpr uint64_t kTsi = 4242;
constexpr unsigned short kMtu = 1400;
constexpr size_t kTunnelPayloadLimit = kMtu - 20 - 8;

struct TunnelBridgeStats {
  size_t received_packet_count = 0;
  size_t forwarded_packet_count = 0;
  size_t max_tunnel_payload_size = 0;
  std::string error;
};

struct TunnelRuntime {
  std::atomic<bool> stop_requested = false;
  TunnelBridgeStats stats;
  std::thread thread;
  std::optional<boost::asio::ip::udp::endpoint> endpoint = std::nullopt;
};

struct EndToEndOptions {
  bool tunneled = false;
  std::string expected_location;
};

auto make_test_payload() -> std::vector<char> {
  std::vector<char> payload(5000);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>('A' + (i % 26));
  }
  return payload;
}

auto calculate_checksum_host_order(const uint8_t* data, size_t length) -> uint16_t {
  uint32_t checksum = 0;

  for (size_t i = 0; i + 1 < length; i += 2) {
    checksum += (static_cast<uint32_t>(data[i]) << 8) | static_cast<uint32_t>(data[i + 1]);
  }
  if ((length % 2) != 0) {
    checksum += static_cast<uint32_t>(data[length - 1]) << 8;
  }

  while ((checksum >> 16U) != 0U) {
    checksum = (checksum & 0xFFFFU) + (checksum >> 16U);
  }

  return static_cast<uint16_t>(~checksum & 0xFFFFU);
}

auto calculate_ipv4_header_checksum(const iphdr& header, size_t header_length) -> uint16_t {
  std::vector<uint8_t> bytes(header_length);
  std::memcpy(bytes.data(), &header, header_length);

  auto* header_copy = reinterpret_cast<iphdr*>(bytes.data());
  header_copy->check = 0;

  return calculate_checksum_host_order(bytes.data(), bytes.size());
}

auto calculate_udp_checksum(const iphdr& ip_header, const udphdr& udp_header, const uint8_t* payload,
                            size_t payload_length) -> uint16_t {
  const size_t udp_length = sizeof(udphdr) + payload_length;
  std::vector<uint8_t> bytes(12 + udp_length);

  std::memcpy(bytes.data(), &ip_header.saddr, sizeof(ip_header.saddr));
  std::memcpy(bytes.data() + 4, &ip_header.daddr, sizeof(ip_header.daddr));
  bytes[8] = 0;
  bytes[9] = static_cast<uint8_t>(ip_header.protocol);
  bytes[10] = static_cast<uint8_t>((udp_length >> 8) & 0xFFU);
  bytes[11] = static_cast<uint8_t>(udp_length & 0xFFU);

  std::memcpy(bytes.data() + 12, &udp_header, sizeof(udphdr));
  auto* udp_header_copy = reinterpret_cast<udphdr*>(bytes.data() + 12);
  udp_header_copy->uh_sum = 0;
  std::memcpy(bytes.data() + 12 + sizeof(udphdr), payload, payload_length);

  return calculate_checksum_host_order(bytes.data(), bytes.size());
}

auto set_socket_timeout(int socket_fd, std::chrono::milliseconds timeout) -> void {
  timeval socket_timeout{};
  socket_timeout.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  socket_timeout.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
  if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout)) != 0) {
    throw std::runtime_error("Failed to set tunnel bridge socket timeout");
  }
}

auto run_tunnel_bridge(std::promise<void> ready_promise, std::atomic<bool>& stop_requested,
                       TunnelBridgeStats& stats) -> void {
  int receive_socket = -1;
  int forward_socket = -1;

  try {
    receive_socket = socket(AF_INET, SOCK_DGRAM, 0);
    forward_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (receive_socket < 0 || forward_socket < 0) {
      throw std::runtime_error("Failed to open tunnel bridge sockets");
    }

    const int reuse = 1;
    if (setsockopt(receive_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
      throw std::runtime_error("Failed to set tunnel bridge socket reuse");
    }

    sockaddr_in receive_address{};
    receive_address.sin_family = AF_INET;
    receive_address.sin_port = htons(kTunnelPort);
    receive_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(receive_socket, reinterpret_cast<const sockaddr*>(&receive_address), sizeof(receive_address)) != 0) {
      throw std::runtime_error("Failed to bind tunnel bridge socket");
    }

    set_socket_timeout(receive_socket, 200ms);
    ready_promise.set_value();

    sockaddr_in forward_address{};
    forward_address.sin_family = AF_INET;
    forward_address.sin_port = htons(kPort);
    forward_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const uint32_t expected_destination =
        htonl(boost::asio::ip::make_address_v4("239.255.0.1").to_uint());
    std::vector<uint8_t> buffer(2048);

    while (!stop_requested.load()) {
      sockaddr_in sender_address{};
      socklen_t sender_length = sizeof(sender_address);
      const ssize_t received_bytes = recvfrom(receive_socket,
                                              buffer.data(),
                                              buffer.size(),
                                              0,
                                              reinterpret_cast<sockaddr*>(&sender_address),
                                              &sender_length);
      if (received_bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          continue;
        }
        throw std::runtime_error("Tunnel bridge failed to receive packet");
      }

      const size_t packet_size = static_cast<size_t>(received_bytes);
      stats.received_packet_count += 1;
      stats.max_tunnel_payload_size = std::max(stats.max_tunnel_payload_size, packet_size);

      if (packet_size > kTunnelPayloadLimit) {
        throw std::runtime_error("Tunnelled packet exceeded MTU-safe payload size");
      }
      if (packet_size < sizeof(iphdr) + sizeof(udphdr)) {
        throw std::runtime_error("Tunnelled packet too short for inner IP/UDP headers");
      }

      const auto* ip_header = reinterpret_cast<const iphdr*>(buffer.data());
      const size_t ip_header_length = static_cast<size_t>(ip_header->ihl) * 4U;
      if (ip_header->version != 4 || ip_header_length < sizeof(iphdr)) {
        throw std::runtime_error("Tunnelled packet contained invalid inner IPv4 header");
      }
      if (packet_size < ip_header_length + sizeof(udphdr)) {
        throw std::runtime_error("Tunnelled packet shorter than declared inner IP header");
      }
      if (ntohs(ip_header->tot_len) != packet_size) {
        throw std::runtime_error("Tunnelled packet inner IPv4 total length mismatch");
      }
      if (ip_header->protocol != IPPROTO_UDP) {
        throw std::runtime_error("Tunnelled packet inner protocol was not UDP");
      }
      if (ip_header->daddr != expected_destination) {
        throw std::runtime_error("Tunnelled packet inner IPv4 destination mismatch");
      }
      if (calculate_ipv4_header_checksum(*ip_header, ip_header_length) != ntohs(ip_header->check)) {
        throw std::runtime_error("Tunnelled packet inner IPv4 checksum mismatch");
      }

      const auto* udp_header = reinterpret_cast<const udphdr*>(buffer.data() + ip_header_length);
      const size_t udp_length = ntohs(udp_header->uh_ulen);
      if (udp_length < sizeof(udphdr) || ip_header_length + udp_length != packet_size) {
        throw std::runtime_error("Tunnelled packet inner UDP length mismatch");
      }
      if (ntohs(udp_header->uh_sport) != kPort || ntohs(udp_header->uh_dport) != kPort) {
        throw std::runtime_error("Tunnelled packet inner UDP ports mismatch");
      }
      if (udp_header->uh_sum == 0) {
        throw std::runtime_error("Tunnelled packet inner UDP checksum missing");
      }

      const size_t flute_payload_size = udp_length - sizeof(udphdr);
      const auto* flute_payload = buffer.data() + ip_header_length + sizeof(udphdr);
      if (calculate_udp_checksum(*ip_header, *udp_header, flute_payload, flute_payload_size) !=
          ntohs(udp_header->uh_sum)) {
        throw std::runtime_error("Tunnelled packet inner UDP checksum mismatch");
      }

      const ssize_t forwarded_bytes = sendto(forward_socket,
                                             flute_payload,
                                             flute_payload_size,
                                             0,
                                             reinterpret_cast<const sockaddr*>(&forward_address),
                                             sizeof(forward_address));
      if (forwarded_bytes != static_cast<ssize_t>(flute_payload_size)) {
        throw std::runtime_error("Tunnel bridge failed to forward FLUTE payload");
      }

      stats.forwarded_packet_count += 1;
    }
  } catch (const std::exception& ex) {
    if (stats.error.empty()) {
      stats.error = ex.what();
    }
    stop_requested.store(true);
    try {
      ready_promise.set_value();
    } catch (const std::future_error&) {
    }
  }

  if (receive_socket >= 0) {
    close(receive_socket);
  }
  if (forward_socket >= 0) {
    close(forward_socket);
  }
}

auto start_tunnel_bridge(TunnelRuntime& tunnel_runtime) -> void {
  std::future<void> tunnel_ready_future;
  std::promise<void> tunnel_ready_promise;
  tunnel_ready_future = tunnel_ready_promise.get_future();
  tunnel_runtime.endpoint =
      boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), kTunnelPort);
  tunnel_runtime.thread =
      std::thread([ready_promise = std::move(tunnel_ready_promise), &tunnel_runtime]() mutable {
        run_tunnel_bridge(std::move(ready_promise), tunnel_runtime.stop_requested, tunnel_runtime.stats);
      });

  const auto tunnel_ready = tunnel_ready_future.wait_for(2s);
  if (tunnel_ready != std::future_status::ready) {
    tunnel_runtime.stop_requested.store(true);
    if (tunnel_runtime.thread.joinable()) {
      tunnel_runtime.thread.join();
    }
    FAIL() << "Timed out waiting for tunnel bridge to start";
  }

  ASSERT_TRUE(tunnel_runtime.stats.error.empty()) << tunnel_runtime.stats.error;
}

auto run_end_to_end_scenario(const EndToEndOptions& options) -> void {
  const std::string expected_location = options.expected_location;
  const std::vector<char> expected_payload = make_test_payload();
  const auto now = std::chrono::system_clock::now();

  ASSERT_FALSE(expected_payload.empty());

  TunnelRuntime tunnel_runtime;
  if (options.tunneled) {
    start_tunnel_bridge(tunnel_runtime);
  }

  boost::asio::io_context receiver_io;
  boost::asio::io_context transmitter_io;

  LibFlute::Receiver receiver("0.0.0.0", "239.255.0.1", kPort, kTsi, receiver_io);
  LibFlute::Transmitter transmitter(
      "239.255.0.1",
      kPort,
      kTsi,
      kMtu,
      0,
      transmitter_io,
      tunnel_runtime.endpoint,
      LibFlute::FileDeliveryTable::FDT_NS_DRAFT_2005);

  auto file_description = std::make_shared<LibFlute::Transmitter::FileDescription>(
      expected_location,
      expected_payload);
  file_description->set_content_type("application/octet-stream");
  file_description->set_expiry_time(now + 60s);

  std::promise<std::shared_ptr<LibFlute::File>> received_file_promise;
  std::promise<uint32_t> transmitted_toi_promise;
  auto received_file_future = received_file_promise.get_future();
  auto transmitted_toi_future = transmitted_toi_promise.get_future();

  receiver.register_completion_callback(
      [&received_file_promise, &receiver, &receiver_io](const std::shared_ptr<LibFlute::File>& file) {
        std::cout << "Received file TOI " << file->meta().toi
                  << " location '" << file->meta().content_location << "'"
                  << " with " << file->length() << " bytes" << std::endl;
        received_file_promise.set_value(file);
        receiver.stop();
        receiver_io.stop();
      });

  transmitter.register_completion_callback(
      [&transmitted_toi_promise, &transmitter, &transmitter_io](const uint32_t toi) {
        std::cout << "Transmitted file TOI " << toi << std::endl;
        transmitted_toi_promise.set_value(toi);
        transmitter.deactivate();
        transmitter_io.stop();
      });

  std::thread receiver_thread([&receiver_io]() { receiver_io.run(); });
  std::thread transmitter_thread([&transmitter_io]() { transmitter_io.run(); });

  std::cout << "Sending payload as '" << expected_location << "' with " << expected_payload.size()
            << " bytes" << (options.tunneled ? " through UDP tunnel" : "") << std::endl;
  transmitter.send(file_description);

  const auto transmitted_ready = transmitted_toi_future.wait_for(5s);
  const auto received_ready = received_file_future.wait_for(5s);

  if (transmitted_ready != std::future_status::ready || received_ready != std::future_status::ready) {
    std::cerr << "Timed out waiting for end-to-end completion. transmitted_ready="
              << (transmitted_ready == std::future_status::ready)
              << ", received_ready=" << (received_ready == std::future_status::ready) << std::endl;
    transmitter.deactivate();
    transmitter_io.stop();
    receiver.stop();
    receiver_io.stop();
  }

  if (receiver_thread.joinable()) {
    receiver_thread.join();
  }
  if (transmitter_thread.joinable()) {
    transmitter_thread.join();
  }

  if (options.tunneled) {
    tunnel_runtime.stop_requested.store(true);
    if (tunnel_runtime.thread.joinable()) {
      tunnel_runtime.thread.join();
    }
  }

  ASSERT_EQ(transmitted_ready, std::future_status::ready);
  ASSERT_EQ(received_ready, std::future_status::ready);

  const auto transmitted_toi = transmitted_toi_future.get();
  const auto received_file = received_file_future.get();
  ASSERT_NE(received_file, nullptr);

  EXPECT_EQ(transmitted_toi, file_description->toi());
  EXPECT_EQ(received_file->meta().toi, transmitted_toi);
  EXPECT_EQ(received_file->meta().content_location, expected_location);
  EXPECT_EQ(received_file->meta().content_length, expected_payload.size());

  const std::string expected_payload_string(expected_payload.begin(), expected_payload.end());
  const std::string received_payload(received_file->buffer(), received_file->length());
  EXPECT_EQ(received_payload, expected_payload_string);

  if (options.tunneled) {
    ASSERT_TRUE(tunnel_runtime.stats.error.empty()) << tunnel_runtime.stats.error;
    EXPECT_GT(tunnel_runtime.stats.received_packet_count, 0U);
    EXPECT_GT(tunnel_runtime.stats.forwarded_packet_count, 0U);
    EXPECT_LE(tunnel_runtime.stats.max_tunnel_payload_size, kTunnelPayloadLimit);
  }
}

}  // namespace

TEST(FluteEndToEndTest, TransmitsFileToReceiver) {
  EndToEndOptions options;
  options.tunneled = false;
  options.expected_location = "e2e/payload.bin";
  run_end_to_end_scenario(options);
}

TEST(FluteEndToEndTest, TransmitsFileToReceiverThroughUdpTunnel) {
  EndToEndOptions options;
  options.tunneled = true;
  options.expected_location = "e2e/tunnelled-payload.bin";
  run_end_to_end_scenario(options);
}
