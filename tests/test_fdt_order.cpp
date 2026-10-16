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
// An object queued on a running Transmitter is sent after the FDT Instance that describes it.
//
// RFC 3926 section 3.4.1: "It is RECOMMENDED that FDT Instance that contains the file description
// entry for a file is sent prior to the sending of the described file within a file delivery
// session."
//
// The send loop runs on the io_context thread while objects are queued from this one, so the
// ordering depends on what the queueing code does first. The packets are taken from the
// Transmitter's UDP tunnel, which carries each one whole (IP and UDP headers included).

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <list>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Transmitter.h"

namespace
{
    using namespace std::chrono_literals;
    using boost::asio::ip::udp;

    constexpr short port = 18097;
    constexpr short tunnel_port = 18098;
    constexpr uint64_t tsi = 77;
    constexpr int objects = 400;

    struct Lct
    {
        uint32_t toi = 0;
        std::optional<uint32_t> fdt_instance;  // EXT_FDT (HET 192), RFC 3926 section 3.4.1
    };

    // RFC 3451 section 5.1: the header length and field widths needed to reach TOI and the
    // header extensions. Returns nothing for a datagram that is not a whole LCT header.
    std::optional<Lct> parse(const std::vector<uint8_t>& d, size_t n)
    {
        if (n < 20 + 8 + 4) return std::nullopt;
        const size_t ip = (d[0] & 0x0f) * 4;
        const uint8_t* p = d.data() + ip + 8;
        const size_t len = n - ip - 8;
        if (len < 4) return std::nullopt;
        const unsigned C = (p[0] >> 2) & 0x3;
        const unsigned S = (p[1] >> 7) & 0x1;
        const unsigned O = (p[1] >> 5) & 0x3;
        const unsigned H = (p[1] >> 4) & 0x1;
        const size_t hdr = p[2] * 4u;
        if (hdr > len) return std::nullopt;
        size_t off = 4 + 4 * (C + 1);
        off += 4 * S + 2 * H;
        const size_t toi_len = 4 * O + 2 * H;
        Lct out;
        for (size_t i = 0; i < toi_len; ++i) out.toi = (out.toi << 8) | p[off + i];
        off += toi_len;
        while (off + 4 <= hdr) {
            const uint8_t het = p[off];
            const size_t ext = het >= 128 ? 4 : p[off + 1] * 4u;
            if (ext == 0) break;
            if (het == 192) out.fdt_instance = ((p[off + 1] & 0x0f) << 16) | (p[off + 2] << 8) | p[off + 3];
            off += ext;
        }
        return out;
    }
}

TEST(FdtOrderTest, ObjectQueuedOnARunningTransmitterFollowsItsFdtInstance)
{
    boost::asio::io_context capture_io;
    udp::socket capture(capture_io, udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), tunnel_port));

    std::mutex m;
    std::condition_variable cv;
    std::vector<Lct> packets;
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        std::vector<uint8_t> buf(65536);
        capture.non_blocking(true);
        while (!stop.load()) {
            boost::system::error_code ec;
            const size_t n = capture.receive(boost::asio::buffer(buf), 0, ec);
            if (ec == boost::asio::error::would_block) { std::this_thread::sleep_for(100us); continue; }
            if (ec) break;
            if (auto l = parse(buf, n)) {
                std::lock_guard<std::mutex> g(m);
                packets.push_back(*l);
                cv.notify_all();
            }
        }
    });

    boost::asio::io_context io;
    auto work = boost::asio::make_work_guard(io);
    LibFlute::Transmitter tx("239.255.0.7", port, tsi, 1400, 0, io,
                             udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), tunnel_port));
    // Every object stays in the FDT, so each new FDT Instance is longer and takes longer to build:
    // a wider gap between queueing an object and the FDT Instance that describes it, if that is the
    // order the Transmitter uses.
    tx.retain_transmitted_in_fdt(true);
    std::set<uint32_t> transmitted;
    tx.register_completion_callback([&](uint32_t toi) {
        std::lock_guard<std::mutex> g(m);
        transmitted.insert(toi);
        cv.notify_all();
    });
    std::thread io_thread([&] { io.run(); });

    std::list<std::vector<char>> contents;
    std::vector<uint32_t> tois;
    std::vector<size_t> queued_at;           // packets captured before each object was queued
    std::vector<uint32_t> newest_instance;   // newest FDT Instance ID captured before then
    for (int i = 0; i < objects; ++i) {
        size_t before;
        uint32_t newest = 0;
        {
            // Wait until the previous object is out and the capture has caught up, so every packet
            // captured from here on was sent after this object was queued.
            std::unique_lock<std::mutex> lk(m);
            if (!tois.empty()) cv.wait_for(lk, 2s, [&] { return transmitted.count(tois.back()) > 0; });
        }
        std::this_thread::sleep_for(3ms);
        {
            std::lock_guard<std::mutex> g(m);
            before = packets.size();
            for (const auto& p : packets) if (p.fdt_instance && *p.fdt_instance > newest) newest = *p.fdt_instance;
        }
        contents.emplace_back(200, static_cast<char>('a' + i % 26));
        auto fd = std::make_shared<LibFlute::Transmitter::FileDescription>("o" + std::to_string(i), contents.back());
        queued_at.push_back(before);
        newest_instance.push_back(newest);
        tois.push_back(tx.send(fd));
    }
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, 5s, [&] { return transmitted.count(tois.back()) > 0; });
    }
    std::this_thread::sleep_for(20ms);
    tx.deactivate();
    work.reset();
    io.stop();
    io_thread.join();
    stop = true;
    reader.join();

    std::lock_guard<std::mutex> g(m);
    int early = 0;
    for (size_t k = 0; k < tois.size(); ++k) {
        // The FDT Instance describing the object is the first one newer than any seen before it
        // was queued; the object's first packet must come after it.
        std::optional<size_t> first_fdt, first_obj;
        for (size_t i = queued_at[k]; i < packets.size(); ++i) {
            const auto& p = packets[i];
            if (!first_fdt && p.toi == 0 && p.fdt_instance && *p.fdt_instance > newest_instance[k]) first_fdt = i;
            if (!first_obj && p.toi == tois[k]) first_obj = i;
        }
        ASSERT_TRUE(first_obj.has_value()) << "TOI " << tois[k] << " never sent";
        if (!first_fdt || *first_fdt > *first_obj) ++early;
    }
    EXPECT_EQ(early, 0) << early << " of " << tois.size()
                        << " objects had a packet sent before the FDT Instance describing them";
}
