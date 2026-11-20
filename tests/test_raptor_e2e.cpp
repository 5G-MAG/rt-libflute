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
// Real Transmitter -> UDP multicast socket -> real Receiver, with Raptor/
// RaptorQ actually enabled -- unlike test_raptor_fec.cpp/test_raptorq_fec.cpp,
// which exercise the codecs and File/EncodingSymbol entirely in-process, this
// is the one thing that was *not* covered anywhere else: real wire
// serialization of Raptor/RaptorQ encoding symbols over an actual socket,
// real FileDeliveryTable XML generation/parsing carrying the FEC-OTI-Number-
// Of-Source-Blocks/Sub-Blocks/Symbol-Alignment-Parameter attributes (the
// codec-level and File-level tests never serialize to XML at all -- they
// pass a FileEntry struct directly from encoder to decoder in memory), and
// real repair-symbol generation/consumption through Transmitter/Receiver's
// actual send/receive loops, not a hand-rolled loop in a test. Mirrors
// test_end_to_end.cpp's existing pattern.
#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <random>
#include <thread>

#include "Receiver.h"
#include "Transmitter.h"

using namespace std::chrono_literals;

namespace {

// Runs one real Transmitter -> multicast socket -> real Receiver transfer
// with the given FEC scheme and returns the received file (or fails the
// test via ASSERT/EXPECT and returns nullptr).
std::shared_ptr<LibFlute::File> run_transfer(
    const std::string& mcast_addr, short port, uint64_t tsi,
    const std::optional<LibFlute::FecOti>& fec_oti,
    const std::string& expected_payload) {
  boost::asio::io_context receiver_io;
  boost::asio::io_context transmitter_io;

  LibFlute::Receiver receiver("0.0.0.0", mcast_addr, port, tsi, receiver_io);
  /* General FLUTE, not the MBMS Download Profile. RaptorQ is not among the schemes TS 26.346
     clause L.4.7 admits, so a RaptorQ session is by definition a general-FLUTE one and the profile
     refuses it at construction. Raptor would be accepted either way; both run here under the same
     profile so the two cases stay comparable. */
  LibFlute::Transmitter transmitter(
      mcast_addr, port, tsi, 1400, 0, transmitter_io, std::nullopt,
      LibFlute::FileDeliveryTable::FDT_NS_DRAFT_2005, true, std::nullopt, fec_oti,
      LibFlute::Profile::Unprofiled);

  auto file_description = std::make_shared<LibFlute::Transmitter::FileDescription>(
      "e2e/payload.bin", expected_payload.c_str(), expected_payload.size());
  file_description->set_content_type("application/octet-stream");
  file_description->set_expiry_time(std::chrono::system_clock::now() + 60s);

  std::promise<std::shared_ptr<LibFlute::File>> received_file_promise;
  std::promise<uint32_t> transmitted_toi_promise;
  auto received_file_future = received_file_promise.get_future();
  auto transmitted_toi_future = transmitted_toi_promise.get_future();

  receiver.register_completion_callback(
      [&received_file_promise, &receiver, &receiver_io](const std::shared_ptr<LibFlute::File>& file) {
        received_file_promise.set_value(file);
        receiver.stop();
        receiver_io.stop();
      });
  transmitter.register_completion_callback(
      [&transmitted_toi_promise, &transmitter, &transmitter_io](uint32_t toi) {
        transmitted_toi_promise.set_value(toi);
        transmitter.deactivate();
        transmitter_io.stop();
      });

  std::thread receiver_thread([&receiver_io]() { receiver_io.run(); });
  std::thread transmitter_thread([&transmitter_io]() { transmitter_io.run(); });

  transmitter.send(file_description);

  auto transmitted_ready = transmitted_toi_future.wait_for(10s);
  auto received_ready = received_file_future.wait_for(10s);

  if (transmitted_ready != std::future_status::ready || received_ready != std::future_status::ready) {
    ADD_FAILURE() << "Timed out. transmitted_ready=" << (transmitted_ready == std::future_status::ready)
                  << " received_ready=" << (received_ready == std::future_status::ready);
    transmitter.deactivate();
    transmitter_io.stop();
    receiver.stop();
    receiver_io.stop();
  }
  if (receiver_thread.joinable()) receiver_thread.join();
  if (transmitter_thread.joinable()) transmitter_thread.join();

  if (received_ready != std::future_status::ready) return nullptr;
  return received_file_future.get();
}

std::string make_payload(size_t len, uint32_t seed) {
  std::mt19937 rng(seed);
  std::string s(len, '\0');
  for (auto& c : s) c = (char)rng();
  return s;
}

} // namespace

TEST(RaptorE2ETest, RealTransmitterToReceiverOverMulticast) {
  // Large enough for several source symbols plus real repair-symbol
  // generation at the default MTU (1400), not just one or two.
  std::string payload = make_payload(30000, 42);
  LibFlute::FecOti fec_oti{.encoding_id = LibFlute::FecScheme::Raptor, .max_source_block_length = 100};

  auto received = run_transfer("239.255.0.2", 18092, 4243, fec_oti, payload);
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(received->meta().fec_oti.encoding_id, LibFlute::FecScheme::Raptor);
  EXPECT_GT(received->meta().fec_oti.nof_source_blocks, 0u); // survived real FDT XML round-trip
  ASSERT_EQ(received->length(), payload.size());
  EXPECT_EQ(std::string(received->buffer(), received->length()), payload);
}

TEST(RaptorQE2ETest, RealTransmitterToReceiverOverMulticast) {
  std::string payload = make_payload(30000, 43);
  LibFlute::FecOti fec_oti{.encoding_id = LibFlute::FecScheme::RaptorQ, .max_source_block_length = 100};

  auto received = run_transfer("239.255.0.3", 18093, 4244, fec_oti, payload);
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(received->meta().fec_oti.encoding_id, LibFlute::FecScheme::RaptorQ);
  EXPECT_GT(received->meta().fec_oti.nof_source_blocks, 0u);
  ASSERT_EQ(received->length(), payload.size());
  EXPECT_EQ(std::string(received->buffer(), received->length()), payload);
}
