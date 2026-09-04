// libflute - FLUTE/ALC library
//
// Copyright (C) 2026 5G-MAG Association (Jordi J. Gimenez <gimenez@5g-mag.com>)
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
// Coverage for object-carousel delivery: a receiver that joins a running carousel
// must be able to acquire every object the carousel repeats, not just whichever
// few happen to be described when it arrives.
//
// A receiver can only recover an object whose TOI an FDT Instance describes, so
// this is really a statement about the FDT. Two properties have to hold together,
// and each is asserted separately below:
//
//   - every carouselled object is described (an object dropped from the table as
//     soon as it finishes sending is undescribed for the rest of the session), and
//   - the FDT that describes them all actually arrives, which for a table covering
//     a whole carousel means one spanning several encoding symbols.
//
// The rate limit is what makes the second property biting rather than theoretical.
// Over a fast path a multi-symbol FDT finishes sending before anything can disturb
// it, so a defect that restarts its transmission stays invisible; under a rate
// limit the same defect keeps a receiver from ever assembling a complete instance.
// These tests therefore run rate-limited, which is the condition a real broadcast
// bearer imposes and a loopback does not.
#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Receiver.h"
#include "Transmitter.h"

namespace {

using namespace std::chrono_literals;

constexpr short kPort = 18097;
constexpr uint64_t kTsi = 4711;
constexpr unsigned short kMtu = 1400;
// In kbps. Matches the order of magnitude a real broadcast bearer offers, and is
// low enough that a multi-symbol FDT takes several packet slots to go out.
constexpr uint32_t kRateLimitKbps = 300;
const std::string kMulticastAddress = "239.255.0.7";
const std::string kReceiverInterface = "0.0.0.0";

// Enough objects that describing all of them cannot fit in one encoding symbol.
// A DASH presentation is the motivating case: a manifest, one initialisation
// segment per representation, and a run of media segments.
constexpr int kObjectCount = 12;

auto object_location(int index) -> std::string
{
  // Deliberately realistic in length: an FDT entry carries the absolute
  // Content-Location, so path length is what decides how many entries fit in a
  // symbol, and short synthetic names would understate the table's real size.
  return "http://192.0.2.10:3004/carousel-under-test/chunk-stream1-" +
         std::string(5 - std::to_string(index).size(), '0') + std::to_string(index) + ".m4s";
}

auto object_payload(int index) -> std::vector<char>
{
  std::vector<char> payload(600 + index * 40);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>('A' + ((index + static_cast<int>(i)) % 26));
  }
  return payload;
}

struct CarouselFixture {
  boost::asio::io_context receiver_io;
  boost::asio::io_context transmitter_io;
  std::mutex mutex;
  std::set<std::string> received_locations;
  std::atomic<bool> stop_carousel{false};
};

}  // namespace

// Each profile pairs with the FDT schema it is defined against, so the sweep below exercises the
// real combinations rather than one profile against every namespace.
struct ProfileCase {
  const char* name;
  LibFlute::Profile profile;
  LibFlute::FileDeliveryTable::FdtNamespace fdt_namespace;
  short port_offset;
};

class FdtCarouselProfileTest : public ::testing::TestWithParam<ProfileCase> {};

INSTANTIATE_TEST_SUITE_P(
    AllProfiles, FdtCarouselProfileTest,
    ::testing::Values(
        ProfileCase{"Ts26517", LibFlute::Profile::Ts26517,
                    LibFlute::FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2, 0},
        ProfileCase{"Ts26346", LibFlute::Profile::Ts26346,
                    LibFlute::FileDeliveryTable::FDT_NS_DRAFT_2005, 2},
        ProfileCase{"Unprofiled", LibFlute::Profile::Unprofiled,
                    LibFlute::FileDeliveryTable::FDT_NS_RFC3926, 4}),
    [](const ::testing::TestParamInfo<ProfileCase>& info) { return info.param.name; });

// The property that matters to a receiver: join a carousel already in progress and
// end up with every object it repeats. Failing this, a DASH client is missing its
// manifest or an initialisation segment and cannot play at all, however cleanly the
// remaining segments arrive. Asserted for every profile, because the FDT-identity rule the
// fix rests on is a FLUTE property and not specific to the 3GPP profiles.
TEST_P(FdtCarouselProfileTest, ReceiverAcquiresEveryCarouselledObjectUnderARateLimit)
{
  const ProfileCase& param = GetParam();
  const short port = kPort + param.port_offset;
  CarouselFixture fixture;

  LibFlute::Receiver receiver(kReceiverInterface, kMulticastAddress, port, kTsi, fixture.receiver_io);
  LibFlute::Transmitter transmitter(kMulticastAddress, port, kTsi, kMtu, kRateLimitKbps,
                                    fixture.transmitter_io, std::nullopt,
                                    param.fdt_namespace, /*active=*/true, std::nullopt, std::nullopt,
                                    param.profile);
  // Togglable so this test can be pointed at the pre-fix behaviour, to confirm it actually
  // catches the defect rather than passing either way.
  transmitter.retain_transmitted_in_fdt(std::getenv("FLUTE_TEST_NO_RETAIN") == nullptr);

  receiver.register_completion_callback(
      [&fixture](const std::shared_ptr<LibFlute::File>& file) {
        const std::lock_guard<std::mutex> lock(fixture.mutex);
        fixture.received_locations.insert(file->meta().content_location);
      });

  // FileDescription is zero-copy: it keeps a pointer to the caller's bytes rather than
  // copying them (see its own _data member), so the payloads have to outlive the transmitter.
  std::vector<std::vector<char>> payloads;
  payloads.reserve(kObjectCount);
  for (int i = 0; i < kObjectCount; ++i) payloads.push_back(object_payload(i));

  std::vector<std::shared_ptr<LibFlute::Transmitter::FileDescription>> descriptions;
  descriptions.reserve(kObjectCount);
  for (int i = 0; i < kObjectCount; ++i) {
    auto description = std::make_shared<LibFlute::Transmitter::FileDescription>(
        object_location(i), payloads[i]);
    description->set_content_type("video/mp4");
    description->set_expiry_time(std::chrono::system_clock::now() + 300s);
    descriptions.push_back(description);
  }

  std::thread receiver_thread([&fixture]() { fixture.receiver_io.run(); });
  std::thread transmitter_thread([&fixture]() { fixture.transmitter_io.run(); });

  // Repeat the whole set, the way an object carousel does. Each pass re-sends every
  // object under the TOI it already holds.
  std::thread carousel_thread([&fixture, &transmitter, &descriptions]() {
    while (!fixture.stop_carousel) {
      for (const auto& description : descriptions) {
        if (fixture.stop_carousel) break;
        transmitter.send(description);
      }
      std::this_thread::sleep_for(500ms);
    }
  });

  const auto deadline = std::chrono::steady_clock::now() + 60s;
  size_t acquired = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    {
      const std::lock_guard<std::mutex> lock(fixture.mutex);
      acquired = fixture.received_locations.size();
    }
    if (acquired >= static_cast<size_t>(kObjectCount)) break;
    std::this_thread::sleep_for(200ms);
  }

  fixture.stop_carousel = true;
  carousel_thread.join();
  transmitter.deactivate();
  fixture.transmitter_io.stop();
  receiver.stop();
  fixture.receiver_io.stop();
  transmitter_thread.join();
  receiver_thread.join();

  std::set<std::string> missing;
  for (int i = 0; i < kObjectCount; ++i) {
    if (fixture.received_locations.count(object_location(i)) == 0) {
      missing.insert(object_location(i));
    }
  }

  std::string missing_report;
  for (const auto& location : missing) missing_report += "\n  " + location;
  EXPECT_TRUE(missing.empty())
      << "Receiver never acquired " << missing.size() << " of " << kObjectCount
      << " carouselled objects. An object the receiver was never told about cannot be"
         " recovered however reliably its symbols arrive." << missing_report;
}

// Narrower assertion on the sender alone, so a failure separates "the table never
// described everything" from "the table was described but never arrived".
TEST(FdtCarouselTest, RetainedTableDescribesEveryCarouselledObject)
{
  boost::asio::io_context io;
  LibFlute::Transmitter transmitter(kMulticastAddress, kPort + 1, kTsi, kMtu, /*rate_limit=*/0, io,
                                    std::nullopt, LibFlute::FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2);
  // Togglable purely so this test can also be pointed at the pre-retention behaviour when
  // isolating whether a failure belongs to retention or to the transmit path underneath it.
  transmitter.retain_transmitted_in_fdt(std::getenv("FLUTE_TEST_NO_RETAIN") == nullptr);

  // Unrated here: this test is about the table's contents once everything has been
  // sent, not about delivery timing, so let transmission finish as fast as it can.
  std::atomic<int> completed{0};
  transmitter.register_completion_callback([&completed](uint32_t) { ++completed; });

  std::thread io_thread([&io]() { io.run(); });

  // Zero-copy, as above: these must outlive the sends below.
  std::vector<std::vector<char>> payloads;
  payloads.reserve(kObjectCount);
  for (int i = 0; i < kObjectCount; ++i) payloads.push_back(object_payload(i));

  std::vector<std::shared_ptr<LibFlute::Transmitter::FileDescription>> descriptions;
  for (int i = 0; i < kObjectCount; ++i) {
    auto description = std::make_shared<LibFlute::Transmitter::FileDescription>(
        object_location(i), payloads[i]);
    description->set_content_type("video/mp4");
    description->set_expiry_time(std::chrono::system_clock::now() + 300s);
    descriptions.push_back(description);
    transmitter.send(description);
  }

  const auto deadline = std::chrono::steady_clock::now() + 30s;
  while (completed < kObjectCount && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(50ms);
  }

  const size_t described = transmitter.fdt().file_entries().size();

  transmitter.deactivate();
  io.stop();
  io_thread.join();

  ASSERT_EQ(completed.load(), kObjectCount) << "Transmission did not finish; the table assertion below would be meaningless.";
  EXPECT_EQ(described, static_cast<size_t>(kObjectCount))
      << "Every object still being carouselled must stay described; an entry dropped on"
         " transmission completion leaves that object unrecoverable for the rest of the session.";
}
