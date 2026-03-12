#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "Transmitter.h"

using namespace LibFlute;

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

TEST(TransmitterLifecycleTest, DeferredDeactivationDrainsQueuedFilesAndStopsFutureSends) {
  using namespace std::chrono_literals;

  boost::asio::io_context io;
  auto work_guard = boost::asio::make_work_guard(io);
  Transmitter tx("127.0.0.1", 5000, /*tsi*/1234, /*mtu*/1400, /*rate_limit*/0, io);

  std::promise<void> first_completion_promise;
  std::promise<void> second_completion_promise;
  auto first_completion = first_completion_promise.get_future();
  auto second_completion = second_completion_promise.get_future();

  tx.register_completion_callback(
      [&](const uint32_t toi) {
        if (toi == 1) {
          first_completion_promise.set_value();
        } else if (toi == 2) {
          second_completion_promise.set_value();
        }
      });

  std::thread io_thread([&io]() { io.run(); });

  const std::vector<char> first_payload{'f', 'i', 'r', 's', 't'};
  const std::vector<char> second_payload{'s', 'e', 'c', 'o', 'n', 'd'};
  const auto first_file = std::make_shared<Transmitter::FileDescription>(
      "test/first.bin", first_payload);
  const auto second_file = std::make_shared<Transmitter::FileDescription>(
      "test/second.bin", second_payload);

  EXPECT_EQ(tx.send(first_file), 1);
  tx.deactivate(true);
  EXPECT_EQ(first_completion.wait_for(2s), std::future_status::ready);

  EXPECT_EQ(tx.send(second_file), 2);
  EXPECT_EQ(second_completion.wait_for(250ms), std::future_status::timeout);

  tx.activate();
  EXPECT_EQ(second_completion.wait_for(2s), std::future_status::ready);

  work_guard.reset();
  io.stop();
  io_thread.join();
}
