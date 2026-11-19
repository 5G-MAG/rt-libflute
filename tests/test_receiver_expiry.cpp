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
// An FDT Instance is not used to interpret packets received after it has expired.
//
// TS 26.346 V19.3.0 clause 7.2.9: "For MBMS operation, the UE shall not use a received FDT Instance to interpret packets
// received beyond the expiration time of the FDT Instance."
//
// The Expires of the File element is deliberately not tested: TS 26.346 V19.3.0 no longer defines it in the profiled
// schema (5G-MAG/Standards#238, open).
//
// The datagrams of a transmission are captured from a Transmitter, the Expires of its FDT Instance is rewritten in place
// (10 digits, so the length of the FDT does not change), and the datagrams are replayed to a Receiver in two parts with a
// pause between them.

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <array>
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
    using Datagrams = std::vector<std::vector<uint8_t>>;

    constexpr uint64_t tsi = 78;
    const std::string group = "239.255.0.78";
    constexpr uint64_t ntp_to_unix_offset = 2208988800ULL;

    uint64_t ntp_now()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() +
               ntp_to_unix_offset;
    }

    void arm(udp::socket& socket, udp::endpoint& from, std::array<uint8_t, 2048>& buffer, Datagrams& out)
    {
        socket.async_receive_from(boost::asio::buffer(buffer), from, [&](const boost::system::error_code& ec, size_t n) {
            if (!ec) {
                out.emplace_back(buffer.begin(), buffer.begin() + n);
                arm(socket, from, buffer, out);
            }
        });
    }

    /// The datagrams that a Transmitter sends for one plain object of about six packets.
    Datagrams capture_transmission(short port)
    {
        boost::asio::io_context cio, tio;
        udp::socket socket(cio, udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
        udp::endpoint from;
        std::array<uint8_t, 2048> buffer{};
        Datagrams out;
        arm(socket, from, buffer, out);

        LibFlute::Transmitter tx("127.0.0.1", port, tsi, 1400, 0, tio);
        std::vector<char> data(8000);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<char>((i * 7 + i / 251) & 0xff);
        }
        auto description = std::make_shared<LibFlute::Transmitter::FileDescription>("o.bin", data);
        tx.send(description);

        std::thread sender([&] { tio.run_for(1s); });
        cio.run_for(1200ms);
        tio.stop();
        sender.join();
        return out;
    }

    /// Writes the 10 digits that follow the n-th 'Expires="' of the FDT datagram.
    void set_nth_expires(std::vector<uint8_t>& datagram, int n, uint64_t value)
    {
        const std::string key = "Expires=\"";
        const std::string text(datagram.begin(), datagram.end());
        size_t pos = 0;
        for (int i = 0; i <= n; ++i) {
            pos = text.find(key, pos);
            ASSERT_NE(pos, std::string::npos);
            pos += key.size();
        }
        const std::string digits = std::to_string(value);
        ASSERT_EQ(digits.size(), 10U);
        for (size_t i = 0; i < digits.size(); ++i) {
            datagram[pos + i] = static_cast<uint8_t>(digits[i]);
        }
    }

    /// Replays the datagrams to a Receiver: the FDT and the first packet of the object at once, the others after a pause
    /// of 3.5 seconds. Returns whether the object was handed to the application.
    bool delivered_after_a_pause(const Datagrams& datagrams, short port)
    {
        boost::asio::io_context rio;
        const udp::endpoint tunnel(boost::asio::ip::make_address("127.0.0.1"), port + 1);
        LibFlute::Receiver receiver("0.0.0.0", group, port, tsi, rio, "", tunnel, boost::asio::ip::make_address("127.0.0.1"),
                                    std::nullopt);
        std::atomic<bool> got{false};
        receiver.register_completion_callback([&](std::shared_ptr<LibFlute::File>) { got = true; });
        std::thread rt([&] { rio.run(); });
        std::this_thread::sleep_for(200ms);

        boost::asio::io_context sio;
        udp::socket socket(sio, udp::v4());
        for (size_t i = 0; i < datagrams.size(); ++i) {
            if (i == 2) {
                std::this_thread::sleep_for(3500ms);
            }
            socket.send_to(boost::asio::buffer(datagrams[i]), tunnel);
            std::this_thread::sleep_for(1ms);
        }
        for (int i = 0; i < 10 && !got; ++i) {
            std::this_thread::sleep_for(100ms);
        }
        rio.stop();
        rt.join();
        return got;
    }
} // namespace

TEST(ReceiverExpiryTest, AnObjectIsDeliveredWhileItsFdtInstanceIsValid)
{
    Datagrams datagrams = capture_transmission(21800);
    ASSERT_GE(datagrams.size(), 4U);
    set_nth_expires(datagrams[0], 0, ntp_now() + 60);
    EXPECT_TRUE(delivered_after_a_pause(datagrams, 21810));
}

TEST(ReceiverExpiryTest, PacketsAfterTheExpiryOfTheFdtInstanceAreNotInterpretedWithIt)
{
    Datagrams datagrams = capture_transmission(21820);
    ASSERT_GE(datagrams.size(), 4U);
    set_nth_expires(datagrams[0], 0, ntp_now() + 2);
    EXPECT_FALSE(delivered_after_a_pause(datagrams, 21830));
}
