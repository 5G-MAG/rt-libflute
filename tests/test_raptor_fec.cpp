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
// Unit tests for the from-scratch RFC 5053 Raptor codec (fec/RaptorCodec.h)
// and its integration into LibFlute::File. Two levels, matching how this
// was actually developed and verified:
//  - RaptorCodecTest: the codec in isolation, round-tripping through
//    simulated symbol loss with realistic repair overhead.
//  - FileRaptorTest: the real File/EncodingSymbol wire-format API,
//    end-to-end -- this is what caught the one real bug found during
//    development (a missing zero-pad for a file's final short symbol; the
//    standalone codec test alone never exercised a non-uniform symbol
//    length, so it passed even with that bug present).
#include <gtest/gtest.h>
#include <random>
#include <algorithm>
#include "fec/RaptorCodec.h"
#include "File.h"

using namespace LibFlute;
using namespace LibFlute::Raptor;

namespace {

std::vector<std::vector<uint8_t>> make_source(uint32_t K, size_t T, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<std::vector<uint8_t>> src(K, std::vector<uint8_t>(T));
  for (auto& sym : src) for (auto& b : sym) b = (uint8_t)rng();
  return src;
}

// Encodes K source symbols, then delivers `received_overhead` more than K
// out of a pool of `pool_overhead` extra repair symbols (a random subset,
// so which ones survive is independent of source vs. repair), and checks
// the decoder recovers the original data exactly.
void expect_round_trip(uint32_t K, size_t T, uint32_t seed, uint32_t received_overhead, uint32_t pool_overhead) {
  RaptorCodec encoder(K);
  auto source = make_source(K, T, seed);
  auto intermediate = encoder.compute_intermediate_symbols(source);
  ASSERT_EQ(intermediate.size(), encoder.L());

  for (uint32_t i = 0; i < K; i++) {
    EXPECT_EQ(encoder.generate_encoding_symbol(i, intermediate), source[i]) << "systematic property failed at ESI " << i;
  }

  uint32_t pool_size = K + pool_overhead;
  uint32_t received_count = K + received_overhead;
  ASSERT_LE(received_count, pool_size);

  std::mt19937 rng(seed * 7919u + 1);
  std::vector<uint32_t> pool(pool_size);
  for (uint32_t i = 0; i < pool_size; i++) pool[i] = i;
  std::shuffle(pool.begin(), pool.end(), rng);

  RaptorCodec decoder(K);
  for (uint32_t i = 0; i < received_count; i++) {
    decoder.add_received_symbol(pool[i], encoder.generate_encoding_symbol(pool[i], intermediate));
  }

  ASSERT_TRUE(decoder.can_decode()) << "needs " << decoder.symbols_needed() << " more independent symbols";
  EXPECT_EQ(decoder.decode_source_symbols(), source);
}

} // namespace

TEST(RaptorCodecTest, CanonicalExactlyKInOrderAlwaysDecodes) {
  // Guaranteed by the systematic index table (RFC 5053 Section 5.7): the
  // canonical ESI 0..K-1 set, with zero loss, must always be decodable.
  RaptorCodec encoder(10);
  auto source = make_source(10, 8, 1);
  auto intermediate = encoder.compute_intermediate_symbols(source);
  RaptorCodec decoder(10);
  for (uint32_t i = 0; i < 10; i++) {
    decoder.add_received_symbol(i, encoder.generate_encoding_symbol(i, intermediate));
  }
  ASSERT_TRUE(decoder.can_decode());
  EXPECT_EQ(decoder.decode_source_symbols(), source);
}

TEST(RaptorCodecTest, SmallBlockWithGenerousOverhead) {
  // Small K needs real relative overhead -- Raptor's own decoding-failure
  // characteristic (Pe ~ 0.85 * 0.567^overhead under maximum-likelihood
  // decoding) is materially worse for small K than the asymptotic figures;
  // see RaptorCodec.h's class comment.
  expect_round_trip(10, 8, 15, /*received_overhead=*/2, /*pool_overhead=*/25);
  expect_round_trip(4, 4, 8, /*received_overhead=*/12, /*pool_overhead=*/20);
}

TEST(RaptorCodecTest, MediumBlock) {
  expect_round_trip(100, 16, 3, 15, 40);
  expect_round_trip(100, 16, 4, 25, 60);
}

TEST(RaptorCodecTest, LargerBlockModestOverheadIsAlreadySafe) {
  expect_round_trip(1000, 32, 5, 15, 60);
  expect_round_trip(1000, 32, 6, 30, 100);
}

TEST(FileRaptorTest, EndToEndThroughRealFileApiSingleBlock) {
  const size_t data_len = 20000;
  std::vector<char> data(data_len);
  std::mt19937 rng(123);
  for (auto& b : data) b = (char)rng();

  FecOti oti{
    .encoding_id = FecScheme::Raptor,
    .encoding_symbol_length = 256,
    .max_source_block_length = 200, // single block: Kt=79 <= 200
    .max_number_of_encoding_symbols = 150,
  };

  File encoder(1, oti, "test.bin", "application/octet-stream", 0, data.data(), data_len, true);
  auto symbols = encoder.get_next_symbols(1024 * 1024);
  ASSERT_FALSE(symbols.empty());
  EXPECT_EQ(encoder.fec_oti().nof_source_blocks, 1u);

  File decoder(encoder.meta());

  std::vector<size_t> order(symbols.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::shuffle(order.begin(), order.end(), rng);
  size_t to_deliver = std::min(symbols.size(), (size_t)100); // K(~79) + ~20 overhead
  std::vector<bool> deliver(symbols.size(), false);
  for (size_t i = 0; i < to_deliver; i++) deliver[order[i]] = true;

  for (size_t i = 0; i < symbols.size(); i++) {
    if (deliver[i]) decoder.put_symbol(symbols[i]);
  }

  ASSERT_TRUE(decoder.complete());
  ASSERT_EQ(decoder.length(), data_len);
  EXPECT_EQ(0, memcmp(decoder.buffer(), data.data(), data_len));
}

TEST(FileRaptorTest, EndToEndThroughRealFileApiMultiBlock) {
  // Forces multiple source blocks (K capped well below Kt), exercising
  // per-block codec setup/repair generation independently.
  const size_t data_len = 20000;
  std::vector<char> data(data_len);
  std::mt19937 rng(456);
  for (auto& b : data) b = (char)rng();

  FecOti oti{
    .encoding_id = FecScheme::Raptor,
    .encoding_symbol_length = 256,
    .max_source_block_length = 20,
    .max_number_of_encoding_symbols = 32,
  };

  File encoder(2, oti, "test2.bin", "application/octet-stream", 0, data.data(), data_len, true);
  auto symbols = encoder.get_next_symbols(1024 * 1024);
  ASSERT_GT(encoder.fec_oti().nof_source_blocks, 1u);

  File decoder(encoder.meta());

  std::vector<size_t> order(symbols.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::shuffle(order.begin(), order.end(), rng);
  size_t to_deliver = (size_t)(symbols.size() * 0.92); // small per-block K needs generous overhead
  std::vector<bool> deliver(symbols.size(), false);
  for (size_t i = 0; i < to_deliver; i++) deliver[order[i]] = true;

  for (size_t i = 0; i < symbols.size(); i++) {
    if (deliver[i]) decoder.put_symbol(symbols[i]);
  }

  ASSERT_TRUE(decoder.complete());
  ASSERT_EQ(decoder.length(), data_len);
  EXPECT_EQ(0, memcmp(decoder.buffer(), data.data(), data_len));
}
