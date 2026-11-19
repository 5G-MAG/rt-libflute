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
// An object is handed to the application only when it is complete.
//
// A content-encoded object is complete when all its symbols have arrived, and is checked again after it has been decoded:
// its Content-MD5 is the one of the decoded content. When that check fails the object is reset to incomplete so that it is
// received again, and it must not be handed to the application as it is. TS 26.346 V19.3.0 clause L.4.2 has Content-MD5
// among the File attributes the receiver reads; what the receiver does with a mismatch is not stated there, so this is
// code-derived, no spec claim.

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
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

    constexpr uint64_t tsi = 77;
    const std::string group = "239.255.0.77";

    void arm(udp::socket& socket, udp::endpoint& from, std::array<uint8_t, 2048>& buffer, Datagrams& out)
    {
        socket.async_receive_from(boost::asio::buffer(buffer), from, [&](const boost::system::error_code& ec, size_t n) {
            if (!ec) {
                out.emplace_back(buffer.begin(), buffer.begin() + n);
                arm(socket, from, buffer, out);
            }
        });
    }

    /// The datagrams that a Transmitter sends for one gzip-encoded object, in the first second.
    Datagrams capture_gzip_transmission(short port)
    {
        boost::asio::io_context cio, tio;
        udp::socket socket(cio, udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
        udp::endpoint from;
        std::array<uint8_t, 2048> buffer{};
        Datagrams out;
        arm(socket, from, buffer, out);

        LibFlute::Transmitter tx("127.0.0.1", port, tsi, 1400, 0, tio);
        std::vector<char> data(3000);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<char>('a' + (i / 50) % 26);
        }
        auto description = std::make_shared<LibFlute::Transmitter::FileDescription>("o.bin", data);
        description->set_compression(LibFlute::Transmitter::FileDescription::COMPRESSION_GZIP);
        tx.send(description);

        std::thread sender([&] { tio.run_for(1s); });
        cio.run_for(1200ms);
        tio.stop();
        sender.join();
        return out;
    }

    /// Changes one character of every Content-MD5 value in the FDT datagrams.
    void corrupt_content_md5(Datagrams& datagrams)
    {
        const std::string key = "Content-MD5=\"";
        for (auto& d : datagrams) {
            const std::string text(d.begin(), d.end());
            const auto pos = text.find(key);
            if (pos != std::string::npos) {
                auto& c = d[pos + key.size()];
                c = (c == 'B') ? 'C' : 'B';
            }
        }
    }

    /// Whether the Receiver hands the object to the application when the datagrams reach its tunnel port.
    bool delivered(const Datagrams& datagrams, short port)
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
        for (const auto& d : datagrams) {
            socket.send_to(boost::asio::buffer(d), tunnel);
            std::this_thread::sleep_for(1ms);
        }
        for (int i = 0; i < 15 && !got; ++i) {
            std::this_thread::sleep_for(100ms);
        }
        rio.stop();
        rt.join();
        return got;
    }
} // namespace

TEST(ReceiverChecksumTest, AnEncodedObjectWithItsChecksumIsDelivered)
{
    Datagrams datagrams = capture_gzip_transmission(21710);
    ASSERT_GE(datagrams.size(), 2U);
    EXPECT_TRUE(delivered(datagrams, 21700));
}

TEST(ReceiverChecksumTest, AnEncodedObjectWhoseDecodedContentFailsItsChecksumIsNotDelivered)
{
    Datagrams datagrams = capture_gzip_transmission(21720);
    ASSERT_GE(datagrams.size(), 2U);
    corrupt_content_md5(datagrams);
    EXPECT_FALSE(delivered(datagrams, 21730));
}
