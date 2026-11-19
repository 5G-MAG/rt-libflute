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
// The Receiver reports that the FDT Instance most recently received has expired, with no packet arriving to show it.
//
// TS 26.517 V19.2.0 clause 6.2.4.2 step 1: "The latest time for this is the value of the @Expires attribute indicated in
// the root element of the FDT Instance document." (the end of the MBS Distribution Session, after which the MBS Client
// repairs what is missing). The Receiver only drops an expired instance when the next packet arrives; a session that has
// ended sends none.
//
// The datagrams of a transmission are captured from a Transmitter, the Expires of its FDT Instance is rewritten in place
// (10 digits, so the length of the FDT does not change), and the datagrams are replayed to a Receiver.

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "AlcPacket.h"
#include "Receiver.h"
#include "Transmitter.h"

namespace
{
    using namespace std::chrono_literals;
    using boost::asio::ip::udp;
    using Datagrams = std::vector<std::vector<uint8_t>>;

    constexpr uint64_t tsi = 79;
    const std::string group = "239.255.0.79";
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

    /// The datagrams that a Transmitter sends for one plain object.
    Datagrams capture_transmission(short port)
    {
        boost::asio::io_context cio, tio;
        udp::socket socket(cio, udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
        udp::endpoint from;
        std::array<uint8_t, 2048> buffer{};
        Datagrams out;
        arm(socket, from, buffer, out);

        LibFlute::Transmitter tx("127.0.0.1", port, tsi, 1400, 0, tio);
        std::vector<char> data(8000, 'x');
        auto description = std::make_shared<LibFlute::Transmitter::FileDescription>("o.bin", data);
        tx.send(description);

        std::thread sender([&] { tio.run_for(1s); });
        cio.run_for(1200ms);
        tio.stop();
        sender.join();
        return out;
    }

    /// Writes the 10 digits that follow the first 'Expires="' of the FDT datagram.
    void set_expires(std::vector<uint8_t>& datagram, uint64_t value)
    {
        const std::string key = "Expires=\"";
        const std::string text(datagram.begin(), datagram.end());
        size_t pos = text.find(key);
        ASSERT_NE(pos, std::string::npos);
        pos += key.size();
        std::string digits = std::to_string(value);
        ASSERT_LE(digits.size(), 10U);
        digits.insert(0, 10 - digits.size(), '0');
        for (size_t i = 0; i < digits.size(); ++i) {
            datagram[pos + i] = static_cast<uint8_t>(digits[i]);
        }
    }

    uint32_t instance_id_of(std::vector<uint8_t> datagram)
    {
        LibFlute::AlcPacket packet(reinterpret_cast<char*>(datagram.data()), datagram.size());
        return packet.fdt_instance_id();
    }

    /// The FDT datagram with another FDT Instance ID: the EXT_FDT header extension is one word, 0xC0, the version in the
    /// high nibble of the second byte with the top four bits of the 20-bit ID, then the other sixteen bits.
    std::vector<uint8_t> with_instance_id(std::vector<uint8_t> datagram, uint32_t id)
    {
        const uint32_t before = instance_id_of(datagram);
        for (size_t i = 4; i + 4 <= std::min<size_t>(datagram.size(), 64); i += 4) {
            if (datagram[i] == 0xC0 && (datagram[i + 1] >> 4) == 1 &&
                (static_cast<uint32_t>(datagram[i + 1] & 0x0F) << 16 | datagram[i + 2] << 8 | datagram[i + 3]) == before) {
                datagram[i + 1] = static_cast<uint8_t>(1 << 4 | ((id >> 16) & 0x0F));
                datagram[i + 2] = static_cast<uint8_t>((id >> 8) & 0xFF);
                datagram[i + 3] = static_cast<uint8_t>(id & 0xFF);
                EXPECT_EQ(instance_id_of(datagram), id);
                return datagram;
            }
        }
        ADD_FAILURE() << "no EXT_FDT word found";
        return datagram;
    }

    /// A Receiver on its own thread that records the reports of an expired FDT Instance.
    struct Rig {
        struct Report {
            uint32_t instance_id;
            uint64_t expires;
            std::chrono::steady_clock::time_point at;
        };

        boost::asio::io_context rio;
        udp::endpoint tunnel;
        std::unique_ptr<LibFlute::Receiver> receiver;
        std::thread thread;
        std::mutex mutex;
        std::vector<Report> reports;
        udp::socket socket;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

        explicit Rig(short port, bool register_callback = true)
            : tunnel(boost::asio::ip::make_address("127.0.0.1"), port + 1), socket(sio(), udp::v4())
        {
            receiver = std::make_unique<LibFlute::Receiver>("0.0.0.0", group, port, tsi, rio, "", tunnel,
                                                            boost::asio::ip::make_address("127.0.0.1"), std::nullopt);
            if (register_callback) {
                receiver->register_fdt_expiry_callback([this](uint32_t id, uint64_t expires) {
                    const std::lock_guard<std::mutex> lock(mutex);
                    reports.push_back({id, expires, std::chrono::steady_clock::now()});
                });
            }
            thread = std::thread([this] { rio.run(); });
            std::this_thread::sleep_for(200ms);
            started = std::chrono::steady_clock::now();
        }

        ~Rig()
        {
            rio.stop();
            thread.join();
        }

        static boost::asio::io_context& sio()
        {
            static boost::asio::io_context context;
            return context;
        }

        void send(const std::vector<uint8_t>& datagram)
        {
            socket.send_to(boost::asio::buffer(datagram), tunnel);
        }

        std::vector<Report> seen()
        {
            const std::lock_guard<std::mutex> lock(mutex);
            return reports;
        }

        double seconds_since_start(std::chrono::steady_clock::time_point t) const
        {
            return std::chrono::duration<double>(t - started).count();
        }
    };

    /// The first datagram of a captured transmission is its FDT.
    std::vector<uint8_t> fdt_with_expiry(const Datagrams& datagrams, uint64_t expires)
    {
        std::vector<uint8_t> fdt = datagrams.at(0);
        set_expires(fdt, expires);
        return fdt;
    }
} // namespace

TEST(ReceiverFdtExpiryCallbackTest, TheExpiryIsReportedOnceAfterItsTimeWithNoPacketArriving)
{
    Datagrams datagrams = capture_transmission(21900);
    ASSERT_GE(datagrams.size(), 2U);
    const uint64_t expires = ntp_now() + 2;
    Rig rig(21910);
    rig.send(fdt_with_expiry(datagrams, expires));

    std::this_thread::sleep_for(1500ms);
    EXPECT_TRUE(rig.seen().empty()) << "reported before the instance expired";

    std::this_thread::sleep_for(3500ms);
    const auto reports = rig.seen();
    ASSERT_EQ(reports.size(), 1U) << "once, not repeatedly";
    EXPECT_EQ(reports[0].instance_id, instance_id_of(datagrams.at(0)));
    EXPECT_EQ(reports[0].expires, expires);
}

TEST(ReceiverFdtExpiryCallbackTest, ANewerInstanceReplacesTheWaitForTheOlderOne)
{
    Datagrams datagrams = capture_transmission(22000);
    ASSERT_GE(datagrams.size(), 2U);
    const uint32_t first_id = instance_id_of(datagrams.at(0));
    const uint64_t newer_expires = ntp_now() + 6;
    Rig rig(22010);
    rig.send(fdt_with_expiry(datagrams, ntp_now() + 2));
    std::this_thread::sleep_for(500ms);
    auto newer = with_instance_id(fdt_with_expiry(datagrams, newer_expires), first_id + 1);
    rig.send(newer);

    std::this_thread::sleep_for(3000ms);
    EXPECT_TRUE(rig.seen().empty()) << "the instance that was replaced was reported";

    std::this_thread::sleep_for(4500ms);
    const auto reports = rig.seen();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].instance_id, first_id + 1);
    EXPECT_EQ(reports[0].expires, newer_expires);
}

TEST(ReceiverFdtExpiryCallbackTest, AnInstanceAlreadyExpiredWhenReceivedIsReportedAtOnce)
{
    Datagrams datagrams = capture_transmission(21920);
    ASSERT_GE(datagrams.size(), 2U);
    const uint64_t expires = ntp_now() - 5;
    Rig rig(21930);
    rig.send(fdt_with_expiry(datagrams, expires));

    std::this_thread::sleep_for(1000ms);
    const auto reports = rig.seen();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].expires, expires);
}

TEST(ReceiverFdtExpiryCallbackTest, AnInstanceWithoutAnExpiryIsNotReported)
{
    /* FileDeliveryTable::expired() takes an Expires of 0 as "does not expire". */
    Datagrams datagrams = capture_transmission(22020);
    ASSERT_GE(datagrams.size(), 2U);
    Rig rig(22030);
    rig.send(fdt_with_expiry(datagrams, 0));

    std::this_thread::sleep_for(2500ms);
    EXPECT_TRUE(rig.seen().empty());
}

TEST(ReceiverFdtExpiryCallbackTest, NothingIsReportedAfterStop)
{
    Datagrams datagrams = capture_transmission(21940);
    ASSERT_GE(datagrams.size(), 2U);
    Rig rig(21950);
    rig.send(fdt_with_expiry(datagrams, ntp_now() + 2));
    std::this_thread::sleep_for(300ms);
    rig.receiver->stop();

    std::this_thread::sleep_for(3500ms);
    EXPECT_TRUE(rig.seen().empty());
}

TEST(ReceiverFdtExpiryCallbackTest, AReceiverDestroyedWhileItsExpiredWaitIsQueuedIsNotCalledOn)
{
    Datagrams datagrams = capture_transmission(21960);
    ASSERT_GE(datagrams.size(), 2U);
    std::atomic<int> reported{0};
    {
        boost::asio::io_context rio;
        const udp::endpoint tunnel(boost::asio::ip::make_address("127.0.0.1"), 21971);
        auto receiver = std::make_unique<LibFlute::Receiver>("0.0.0.0", group, 21970, tsi, rio, "", tunnel,
                                                             boost::asio::ip::make_address("127.0.0.1"), std::nullopt);
        receiver->register_fdt_expiry_callback([&](uint32_t, uint64_t) { ++reported; });
        std::thread rt([&] { rio.run(); });
        std::this_thread::sleep_for(200ms);

        boost::asio::io_context sio;
        udp::socket socket(sio, udp::v4());
        const auto fdt = fdt_with_expiry(datagrams, ntp_now() + 2);
        socket.send_to(boost::asio::buffer(fdt), tunnel);
        std::this_thread::sleep_for(300ms);

        /* The receiver is destroyed on the io_context's thread, as a client does when it deactivates a session, while
           the wait for the expiry has already run out and its handler is queued: a timer of the test that runs out
           earlier destroys the receiver, and the thread is kept busy until both have run out, so that both are
           collected together, the test's first. */
        boost::asio::steady_timer destroyer(rio);
        boost::asio::post(rio, [&] {
            destroyer.expires_after(1s);
            destroyer.async_wait([&](const boost::system::error_code&) { receiver.reset(); });
        });
        boost::asio::post(rio, [] { std::this_thread::sleep_for(4s); });
        std::this_thread::sleep_for(5500ms);
        rio.stop();
        rt.join();
        EXPECT_EQ(receiver, nullptr) << "the receiver was not destroyed by the test's own timer";
    }
    EXPECT_EQ(reported.load(), 0);
}

TEST(ReceiverFdtExpiryCallbackTest, AReceiverWithoutTheCallbackIgnoresTheExpiry)
{
    Datagrams datagrams = capture_transmission(21980);
    ASSERT_GE(datagrams.size(), 2U);
    Rig rig(21990, false);
    rig.send(fdt_with_expiry(datagrams, ntp_now() + 1));
    std::this_thread::sleep_for(3000ms);
    SUCCEED();
}
