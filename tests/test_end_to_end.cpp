// libflute - FLUTE/ALC library
//
// Copyright (C) 2026 Fraunhofer FOKUS
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

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "Receiver.h"
#include "Transmitter.h"

namespace {

auto read_fixture_file(const std::filesystem::path& file_path) -> std::string {
  std::ifstream input(file_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open end-to-end fixture file");
  }

  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(FluteEndToEndTest, TransmitsFileToReceiver) {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  constexpr short kPort = 18091;
  const fs::path fixtures_dir = fs::path{__FILE__}.parent_path() / "tmp";
  const fs::path input_file = fixtures_dir / "e2e_payload.bin";
  const std::string expected_location = "e2e/payload.bin";
  const std::string expected_payload = read_fixture_file(input_file);
  const auto now = std::chrono::system_clock::now();

  ASSERT_FALSE(expected_payload.empty());

  boost::asio::io_context receiver_io;
  boost::asio::io_context transmitter_io;

  LibFlute::Receiver receiver("0.0.0.0", "239.255.0.1", kPort, 4242, receiver_io);
  LibFlute::Transmitter transmitter(
      "239.255.0.1",
      kPort,
      4242,
      1400,
      0,
      transmitter_io,
      std::nullopt,
      LibFlute::FileDeliveryTable::FDT_NS_DRAFT_2005);

  auto file_description = std::make_shared<LibFlute::Transmitter::FileDescription>(
      expected_location,
      input_file.string());
  file_description->set_content_type("application/octet-stream");
  file_description->set_expiry_time(now + 60s);

  std::promise<std::shared_ptr<LibFlute::File>> received_file_promise;
  std::promise<uint32_t> transmitted_toi_promise;
  auto received_file_future = received_file_promise.get_future();
  auto transmitted_toi_future = transmitted_toi_promise.get_future();

  receiver.register_completion_callback(
      [&received_file_promise, &receiver, &receiver_io](const std::shared_ptr<LibFlute::File>& file) {
        std::cout << "Received file TOI " << file->meta().toi
                  << " location '" << file->meta().content_location << "'"
                  << " with " << file->length() << " bytes" << std::endl;
        received_file_promise.set_value(file);
        receiver.stop();
        receiver_io.stop();
      });

  transmitter.register_completion_callback(
      [&transmitted_toi_promise, &transmitter, &transmitter_io](const uint32_t toi) {
        std::cout << "Transmitted file TOI " << toi << std::endl;
        transmitted_toi_promise.set_value(toi);
        transmitter.deactivate();
        transmitter_io.stop();
      });

  std::thread receiver_thread([&receiver_io]() { receiver_io.run(); });
  std::thread transmitter_thread([&transmitter_io]() { transmitter_io.run(); });

  std::cout << "Sending fixture '" << input_file.string() << "' as '" << expected_location
            << "' with " << expected_payload.size() << " bytes" << std::endl;
  transmitter.send(file_description);

  const auto transmitted_ready = transmitted_toi_future.wait_for(5s);
  const auto received_ready = received_file_future.wait_for(5s);

  if (transmitted_ready != std::future_status::ready || received_ready != std::future_status::ready) {
    std::cerr << "Timed out waiting for end-to-end completion. transmitted_ready="
              << (transmitted_ready == std::future_status::ready)
              << ", received_ready=" << (received_ready == std::future_status::ready) << std::endl;
    transmitter.deactivate();
    transmitter_io.stop();
    receiver.stop();
    receiver_io.stop();
  }

  if (receiver_thread.joinable()) {
    receiver_thread.join();
  }
  if (transmitter_thread.joinable()) {
    transmitter_thread.join();
  }

  ASSERT_EQ(transmitted_ready, std::future_status::ready);
  ASSERT_EQ(received_ready, std::future_status::ready);

  const auto transmitted_toi = transmitted_toi_future.get();
  const auto received_file = received_file_future.get();
  ASSERT_NE(received_file, nullptr);

  EXPECT_EQ(transmitted_toi, file_description->toi());
  EXPECT_EQ(received_file->meta().toi, transmitted_toi);
  EXPECT_EQ(received_file->meta().content_location, expected_location);
  EXPECT_EQ(received_file->meta().content_length, expected_payload.size());

  const std::string received_payload(received_file->buffer(), received_file->length());
  EXPECT_EQ(received_payload, expected_payload);
}
