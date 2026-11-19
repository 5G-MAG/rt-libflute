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
// Small objects through Raptor, real Transmitter -> UDP multicast socket -> real Receiver, the way the BM-SC sends them (MBMS Download profile, a 1500 byte MTU, only the
// scheme given in the FEC OTI). For an object of a few symbols Raptor shortens the object's own encoding symbol length (so that the source block has the K it needs), and a
// packet then has room for fewer of those symbols than the length alone suggests: the sender must not put in more than fit (File::get_next_symbols).
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <boost/asio.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <random>
#include <thread>
#include <sstream>
#include <vector>

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
    const std::string& expected_payload, unsigned short mtu, LibFlute::Profile profile) {
  boost::asio::io_context receiver_io;
  boost::asio::io_context transmitter_io;

  LibFlute::Receiver receiver("0.0.0.0", mcast_addr, port, tsi, receiver_io);
  /* General FLUTE, not the MBMS Download Profile. RaptorQ is not among the schemes TS 26.346
     clause L.4.7 admits, so a RaptorQ session is by definition a general-FLUTE one and the profile
     refuses it at construction. Raptor would be accepted either way; both run here under the same
     profile so the two cases stay comparable. */
  LibFlute::Transmitter transmitter(
      mcast_addr, port, tsi, mtu, 0, transmitter_io, std::nullopt,
      LibFlute::FileDeliveryTable::FDT_NS_DRAFT_2005, true, std::nullopt, fec_oti,
      profile);

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

// Small objects through Raptor: the positioning assistance file (a few kilobytes) and its neighbours in size, sent the way the BM-SC sends them (MBMS Download profile, a
// 1500 byte MTU, only the scheme given in the FEC OTI).
TEST(RaptorSmallObjects, EverySizeArrives) {
  std::vector<size_t> sizes = {100, 1000, 1400, 1436, 1437, 1500, 2000, 2800, 2872, 3000, 4000, 4308, 5540, 8000};
  if (const char* only = std::getenv("SIZES")) {   // for looking at one size
    sizes.clear();
    std::istringstream in(only);
    for (size_t v; in >> v;) sizes.push_back(v);
  }
  if (std::getenv("SIZES")) spdlog::set_level(spdlog::level::debug);
  int n = 0;
  for (size_t size : sizes) {
    std::string payload = make_payload(size, 7 + n);
    LibFlute::FecOti fec_oti{.encoding_id = LibFlute::FecScheme::Raptor};
    auto received = run_transfer("239.255.0.2", 18100 + n, 4300 + n, fec_oti, payload, 1500, LibFlute::Profile::MBMS::Download);
    n++;
    EXPECT_NE(received, nullptr) << "size " << size;
    if (!received) continue;
    EXPECT_EQ(received->length(), payload.size()) << "size " << size;
    EXPECT_EQ(std::string(received->buffer(), received->length()), payload) << "size " << size;
  }
}
