// libflute - FLUTE/ALC library
//
// Copyright (C) 2026 5G-MAG Association (Jordi J. Gimenez <gimenez@5g-mag.com>)
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
// The Receiver's tunnel reception (issue #66): datagrams arriving on tunnel_address are read
// from the offset the packet_modifier returns, or taken as ALC/LCT packets as they are when no
// packet_modifier is given; tunnel_source admits one sender only, and the multicast socket's
// source_address does not apply to them. A tunnel socket that cannot be bound leaves multicast
// reception working, and tunnel traffic that does not parse is warned about once.

#include <gtest/gtest.h>

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "File.h"
#include "Receiver.h"
#include "Transmitter.h"

namespace
{
    using namespace std::chrono_literals;
    using boost::asio::ip::udp;

    constexpr uint64_t tsi = 66;
    const std::string group = "239.255.0.66";

    // The Transmitter's tunnel mode wraps each ALC packet in an inner IPv4 and UDP header.
    size_t skip_inner_ipv4_udp(std::vector<uint8_t>& d)
    {
        return d.size() > 20 ? (d[0] & 0x0f) * 4 + 8 : SIZE_MAX;
    }

    struct Setup {
        bool tunnelled_sender;                                  // Transmitter in tunnel mode
        std::optional<std::string> tunnel_source;               // admitted by the Receiver
        std::optional<LibFlute::Receiver::packet_modifier_t> modifier;
        std::string source_address = "";                        // the multicast socket's SSM source
    };

    // Sends one object to the Receiver's tunnel port and reports whether it was delivered. A
    // tunnelled sender wraps it; otherwise the sender addresses plain ALC packets to that port.
    bool delivered(const Setup& s, short port)
    {
        boost::asio::io_context rio, tio;
        const udp::endpoint tunnel(boost::asio::ip::make_address("127.0.0.1"), port + 1);
        std::optional<boost::asio::ip::address> source;
        if (s.tunnel_source) source = boost::asio::ip::make_address(*s.tunnel_source);
        LibFlute::Receiver receiver("0.0.0.0", group, port, tsi, rio, s.source_address, tunnel, source, s.modifier);
        std::atomic<bool> got{false};
        receiver.register_completion_callback([&](std::shared_ptr<LibFlute::File>) { got = true; });
        std::thread rt([&] { rio.run(); });
        std::this_thread::sleep_for(200ms);

        std::unique_ptr<LibFlute::Transmitter> tx = s.tunnelled_sender
            ? std::make_unique<LibFlute::Transmitter>(group, port, tsi, 1400, 0, tio, tunnel)
            : std::make_unique<LibFlute::Transmitter>("127.0.0.1", port + 1, tsi, 1400, 0, tio);
        std::vector<char> data(3000, 'x');
        tx->send(std::make_shared<LibFlute::Transmitter::FileDescription>("tunnel.bin", data));
        std::thread tt([&] { tio.run_for(3s); });
        for (int i = 0; i < 30 && !got; ++i) std::this_thread::sleep_for(100ms);
        tio.stop();
        rio.stop();
        tt.join();
        rt.join();
        return got;
    }
}

TEST(ReceiverTunnelTest, WithAPacketModifierTheInnerHeadersAreSkipped)
{
    EXPECT_TRUE(delivered({true, std::string("127.0.0.1"), skip_inner_ipv4_udp}, 21600));
}

TEST(ReceiverTunnelTest, ADatagramFromAnotherTunnelSourceIsNotAdmitted)
{
    EXPECT_FALSE(delivered({true, std::string("192.0.2.1"), skip_inner_ipv4_udp}, 21630));
}

// The multicast socket's source-specific join names 192.0.2.1; tunnel datagrams come from the
// tunnel's own sender and are admitted by tunnel_source, not by that source address.
TEST(ReceiverTunnelTest, TheMulticastSourceAddressDoesNotFilterTunnelDatagrams)
{
    EXPECT_TRUE(delivered({true, std::string("127.0.0.1"), skip_inner_ipv4_udp, "192.0.2.1"}, 21640));
}

TEST(ReceiverTunnelTest, WithoutAPacketModifierDatagramsAreTakenAsTheyAre)
{
    EXPECT_TRUE(delivered({false, std::string("127.0.0.1"), std::nullopt}, 21610));
}

TEST(ReceiverTunnelTest, WithoutAPacketModifierAWrappedDatagramIsNotUnwrapped)
{
    EXPECT_FALSE(delivered({true, std::string("127.0.0.1"), std::nullopt}, 21620));
}

// Still-encapsulated tunnel traffic with no packet_modifier: every datagram fails to parse, and
// the Receiver warns once rather than once per datagram.
TEST(ReceiverTunnelTest, UndecodableTunnelTrafficIsWarnedAboutOnce)
{
    auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(1000);
    auto logger = spdlog::default_logger();
    logger->sinks().push_back(sink);
    EXPECT_FALSE(delivered({true, std::string("127.0.0.1"), std::nullopt}, 21650));
    logger->sinks().pop_back();
    int warnings = 0;
    for (const auto& line : sink->last_formatted())
        if (line.find("Failed to decode") != std::string::npos && line.find("[warning]") != std::string::npos) ++warnings;
    EXPECT_EQ(warnings, 1);
}

// 192.0.2.1 (TEST-NET-1) is not an address of this host, so the tunnel socket cannot be bound;
// the constructor does not throw, and the session still arrives over multicast.
TEST(ReceiverTunnelTest, ATunnelSocketThatCannotBeBoundLeavesMulticastWorking)
{
    boost::asio::io_context rio, tio;
    const short port = 21660;
    const udp::endpoint unbindable(boost::asio::ip::make_address("192.0.2.1"), port + 1);
    std::unique_ptr<LibFlute::Receiver> receiver;
    ASSERT_NO_THROW(receiver = std::make_unique<LibFlute::Receiver>("0.0.0.0", group, port, tsi, rio, "", unbindable));
    std::atomic<bool> got{false};
    receiver->register_completion_callback([&](std::shared_ptr<LibFlute::File>) { got = true; });
    std::thread rt([&] { rio.run(); });
    std::this_thread::sleep_for(200ms);
    LibFlute::Transmitter tx(group, port, tsi, 1400, 0, tio);
    std::vector<char> data(3000, 'm');
    tx.send(std::make_shared<LibFlute::Transmitter::FileDescription>("multicast.bin", data));
    std::thread tt([&] { tio.run_for(3s); });
    for (int i = 0; i < 30 && !got; ++i) std::this_thread::sleep_for(100ms);
    tio.stop();
    rio.stop();
    tt.join();
    rt.join();
    EXPECT_TRUE(got);
    EXPECT_FALSE(receiver->tunnel_active());
}

// With neither a packet_modifier nor a tunnel_source nothing says who may send the datagrams that would
// be taken as ALC/LCT packets as they are, so the tunnel is not enabled (RFC 3450 clause 4.5) and the
// application can see that: plain ALC packets sent to the tunnel port are not delivered.
TEST(ReceiverTunnelTest, WithoutAPacketModifierOrATunnelSourceTheTunnelIsNotEnabled)
{
    boost::asio::io_context rio, tio;
    const short port = 21670;
    const udp::endpoint tunnel(boost::asio::ip::make_address("127.0.0.1"), port + 1);
    LibFlute::Receiver receiver("0.0.0.0", group, port, tsi, rio, "", tunnel);
    EXPECT_FALSE(receiver.tunnel_active());
    std::atomic<bool> got{false};
    receiver.register_completion_callback([&](std::shared_ptr<LibFlute::File>) { got = true; });
    std::thread rt([&] { rio.run(); });
    std::this_thread::sleep_for(200ms);
    LibFlute::Transmitter tx("127.0.0.1", port + 1, tsi, 1400, 0, tio);
    std::vector<char> data(3000, 'x');
    tx.send(std::make_shared<LibFlute::Transmitter::FileDescription>("tunnel.bin", data));
    std::thread tt([&] { tio.run_for(2s); });
    for (int i = 0; i < 20 && !got; ++i) std::this_thread::sleep_for(100ms);
    tio.stop();
    rio.stop();
    tt.join();
    rt.join();
    EXPECT_FALSE(got);
}

// tunnel_active() is true once the tunnel socket is bound.
TEST(ReceiverTunnelTest, ABoundTunnelIsReportedActive)
{
    boost::asio::io_context rio;
    const short port = 21680;
    const udp::endpoint tunnel(boost::asio::ip::make_address("127.0.0.1"), port + 1);
    LibFlute::Receiver with_source("0.0.0.0", group, port, tsi, rio, "", tunnel, boost::asio::ip::make_address("127.0.0.1"));
    EXPECT_TRUE(with_source.tunnel_active());
    const udp::endpoint other(boost::asio::ip::make_address("127.0.0.1"), port + 2);
    LibFlute::Receiver with_modifier("0.0.0.0", group, port, tsi, rio, "", other, std::nullopt, skip_inner_ipv4_udp);
    EXPECT_TRUE(with_modifier.tunnel_active());
}
