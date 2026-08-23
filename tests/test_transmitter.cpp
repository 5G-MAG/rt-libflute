#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <chrono>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include "Transmitter.h"

using namespace LibFlute;

namespace {
  uint16_t read_be16(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }

  /* A second implementation of the same one's-complement sum Transmitter.cpp computes, so the
     transmitted checksum is re-derived here rather than read back from the code under test. */
  uint16_t ones_complement_sum(const uint8_t* buffer, size_t len) {
    uint32_t sum = 0;
    while (len > 1) {
      sum += (static_cast<uint32_t>(buffer[0]) << 8) | buffer[1];
      len -= 2;
      buffer += 2;
    }
    if (len > 0) sum += static_cast<uint32_t>(buffer[0]) << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
  }
}

// Helper to construct a Transmitter for tests
static std::unique_ptr<Transmitter> make_tx(boost::asio::io_context &io, uint32_t rate_limit = 0) {
  // Use a multicast address and reasonable MTU
  return std::make_unique<Transmitter>("239.1.1.1", 5000, /*tsi*/1234, /*mtu*/1400, rate_limit, io);
}

TEST(TransmitterGetterSetterTest, RateLimitGetterSetter) {
  boost::asio::io_context io;
  auto tx = make_tx(io, 0);
  // Initial value should match constructor
  EXPECT_EQ(tx->rate_limit(), 0u);
  // Set new value
  tx->rate_limit(1500);
  EXPECT_EQ(tx->rate_limit(), 1500u);
  // Chaining
  tx->rate_limit(2000).rate_limit(3000);
  EXPECT_EQ(tx->rate_limit(), 3000u);
}

TEST(TransmitterGetterSetterTest, EndpointSetterString) {
  boost::asio::io_context io;
  auto tx = make_tx(io);
  // Initial endpoint
  auto ep_initial = tx->endpoint();
  EXPECT_EQ(ep_initial.address().to_string(), std::string("239.1.1.1"));
  EXPECT_EQ(ep_initial.port(), 5000);
  // Change via string overload
  tx->endpoint("239.1.1.2", 6000);
  auto ep_new = tx->endpoint();
  EXPECT_EQ(ep_new.address().to_string(), std::string("239.1.1.2"));
  EXPECT_EQ(ep_new.port(), 6000);
}

TEST(TransmitterGetterSetterTest, EndpointSetterEndpointObject) {
  boost::asio::io_context io;
  auto tx = make_tx(io);
  boost::asio::ip::udp::endpoint new_ep(boost::asio::ip::make_address("239.1.1.3"), 7000);
  tx->endpoint(new_ep);
  auto ep = tx->endpoint();
  EXPECT_EQ(ep.address().to_string(), std::string("239.1.1.3"));
  EXPECT_EQ(ep.port(), 7000);
  // Move overload
  boost::asio::ip::udp::endpoint moved_ep(boost::asio::ip::make_address("239.1.1.4"), 8000);
  tx->endpoint(std::move(moved_ep));
  auto ep2 = tx->endpoint();
  EXPECT_EQ(ep2.address().to_string(), std::string("239.1.1.4"));
  EXPECT_EQ(ep2.port(), 8000);
}

TEST(TransmitterGetterSetterTest, UdpTunnelAddressSetAndUnset) {
  boost::asio::io_context io;
  auto tx = make_tx(io);
  // Initially no tunnel endpoint
  EXPECT_FALSE(tx->udp_tunnel_address().has_value());
  // Set tunnel endpoint (copy overload)
  boost::asio::ip::udp::endpoint tunnel_ep(boost::asio::ip::make_address("127.0.0.1"), 9000);
  tx->udp_tunnel_address(tunnel_ep);
  ASSERT_TRUE(tx->udp_tunnel_address().has_value());
  EXPECT_EQ(tx->udp_tunnel_address()->address().to_string(), std::string("127.0.0.1"));
  EXPECT_EQ(tx->udp_tunnel_address()->port(), 9000);
  // Set tunnel endpoint (move overload) to a new value
  boost::asio::ip::udp::endpoint tunnel_ep2(boost::asio::ip::make_address("127.0.0.1"), 9100);
  tx->udp_tunnel_address(std::move(tunnel_ep2));
  ASSERT_TRUE(tx->udp_tunnel_address().has_value());
  EXPECT_EQ(tx->udp_tunnel_address()->port(), 9100);
  // Unset
  tx->udp_tunnel_address(std::nullopt);
  EXPECT_FALSE(tx->udp_tunnel_address().has_value());
}

/* The tunnel path's inner IP and UDP headers are built by create_ip_hdr() and create_udp_pkt(),
   both file-local to Transmitter.cpp. This drives them through a real Transmitter with an IPv6
   destination and a UDP tunnel, and parses the bytes it actually sends to a local socket standing
   in for the tunnel peer. */
TEST(TransmitterIPv6TunnelTest, BuildsCorrectInnerIPv6AndUdpHeaders) {
  using namespace std::chrono_literals;

  boost::asio::io_context io;
  auto work_guard = boost::asio::make_work_guard(io);

  // A real local socket standing in for the tunnel peer, so the Transmitter's tunnel-local-address
  // resolution (which connect()s a throwaway socket to the tunnel endpoint to learn its own
  // source address) has something real to connect to.
  boost::asio::ip::udp::socket tunnel_peer(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), 0));
  const auto tunnel_port = tunnel_peer.local_endpoint().port();
  boost::asio::ip::udp::endpoint tunnel_endpoint(boost::asio::ip::make_address("::1"), tunnel_port);

  const std::string destination = "ff3e:30:2001:db8::1234"; // RFC 3306 unicast-prefix-based multicast
  Transmitter tx(destination, 5000, /*tsi*/1234, /*mtu*/1400, /*rate_limit*/0, io, tunnel_endpoint);

  std::vector<uint8_t> received(2048);
  boost::asio::ip::udp::endpoint sender_endpoint;
  std::promise<size_t> received_promise;
  auto received_future = received_promise.get_future();
  tunnel_peer.async_receive_from(boost::asio::buffer(received), sender_endpoint,
      [&](const boost::system::error_code& ec, size_t bytes) {
        if (!ec) received_promise.set_value(bytes);
      });

  const std::vector<char> payload{'i', 'p', 'v', '6', '-', 't', 'u', 'n', 'n', 'e', 'l'};
  auto file = std::make_shared<Transmitter::FileDescription>("test/ipv6-tunnel.bin", payload);
  tx.send(file);

  std::thread io_thread([&io]() { io.run(); });
  ASSERT_EQ(received_future.wait_for(2s), std::future_status::ready);
  size_t bytes_received = received_future.get();
  ASSERT_GE(bytes_received, 40u + 8u); // IPv6 header + UDP header, at minimum

  const uint8_t* ip = received.data();
  /* The fields checked below are RFC 8200 clause 3's, in its order: version nibble, payload length,
     next header, hop limit, then the two addresses.

     RFC 8200 clause 3:
     "Version             4-bit Internet Protocol version number = 6."
  */
  EXPECT_EQ(ip[0] >> 4, 6) << "the version nibble is not 6";
  uint16_t payload_length = read_be16(ip + 4);
  EXPECT_EQ(static_cast<size_t>(payload_length) + 40, bytes_received)
      << "IPv6 payload length field should equal the actual UDP segment size";
  EXPECT_EQ(ip[6], 17) << "Next header should be UDP (17)";
  boost::asio::ip::address_v6::bytes_type src_bytes, dst_bytes;
  std::memcpy(src_bytes.data(), ip + 8, 16);
  std::memcpy(dst_bytes.data(), ip + 24, 16);
  EXPECT_EQ(boost::asio::ip::make_address_v6(dst_bytes), boost::asio::ip::make_address(destination).to_v6())
      << "IPv6 destination address should match the Transmitter's configured endpoint";

  // UDP header + payload, immediately following the 40-byte IPv6 header.
  const uint8_t* udp = ip + 40;
  size_t udp_length = bytes_received - 40;
  EXPECT_EQ(read_be16(udp + 4), udp_length) << "UDP length field should equal the actual segment size";

  // Re-verify the UDP checksum independently: per RFC 8200 clause 8.1, summing the IPv6 pseudo-header
  // plus the UDP segment (including the transmitted checksum field itself, not zeroed) must
  // yield exactly zero for a valid checksum, by the standard one's-complement self-check property.
  std::vector<uint8_t> pseudo_and_segment(40 + udp_length);
  std::memcpy(pseudo_and_segment.data(), src_bytes.data(), 16);
  std::memcpy(pseudo_and_segment.data() + 16, dst_bytes.data(), 16);
  pseudo_and_segment[32] = 0; pseudo_and_segment[33] = 0;
  pseudo_and_segment[34] = static_cast<uint8_t>(udp_length >> 8);
  pseudo_and_segment[35] = static_cast<uint8_t>(udp_length & 0xFF);
  pseudo_and_segment[36] = 0; pseudo_and_segment[37] = 0; pseudo_and_segment[38] = 0;
  pseudo_and_segment[39] = 17; // next header (UDP)
  std::memcpy(pseudo_and_segment.data() + 40, udp, udp_length);
  EXPECT_EQ(ones_complement_sum(pseudo_and_segment.data(), pseudo_and_segment.size()), 0u)
      << "IPv6 UDP pseudo-header checksum should self-verify to zero";

  tx.deactivate();
  work_guard.reset();
  io.stop();
  io_thread.join();
}

TEST(TransmitterLifecycleTest, DeferredDeactivationDrainsQueuedFilesAndStopsFutureSends) {
  using namespace std::chrono_literals;

  boost::asio::io_context io;
  auto work_guard = boost::asio::make_work_guard(io);
  Transmitter tx("127.0.0.1", 5000, /*tsi*/1234, /*mtu*/1400, /*rate_limit*/0, io);

  std::promise<void> first_completion_promise;
  std::promise<void> second_completion_promise;
  std::promise<void> lifecycle_settled_promise;
  auto first_completion = first_completion_promise.get_future();
  auto second_completion = second_completion_promise.get_future();
  auto lifecycle_settled = lifecycle_settled_promise.get_future();

  tx.register_completion_callback(
      [&](const uint32_t toi) {
        if (toi == 1) {
          first_completion_promise.set_value();
          boost::asio::post(io, [&lifecycle_settled_promise]() {
            lifecycle_settled_promise.set_value();
          });
        } else if (toi == 2) {
          second_completion_promise.set_value();
        }
      });

  const std::vector<char> first_payload{'f', 'i', 'r', 's', 't'};
  const std::vector<char> second_payload{'s', 'e', 'c', 'o', 'n', 'd'};
  const auto first_file = std::make_shared<Transmitter::FileDescription>(
      "test/first.bin", first_payload);
  const auto second_file = std::make_shared<Transmitter::FileDescription>(
      "test/second.bin", second_payload);

  EXPECT_EQ(tx.send(first_file), 1);
  tx.deactivate(true);
  tx.activate();

  std::thread io_thread([&io]() { io.run(); });
  EXPECT_EQ(first_completion.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(lifecycle_settled.wait_for(2s), std::future_status::ready);

  EXPECT_EQ(tx.send(second_file), 2);
  EXPECT_EQ(second_completion.wait_for(250ms), std::future_status::timeout);

  tx.activate();
  EXPECT_EQ(second_completion.wait_for(2s), std::future_status::ready);

  work_guard.reset();
  io.stop();
  io_thread.join();
}
