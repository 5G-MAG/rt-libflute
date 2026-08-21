// libflute - FLUTE/ALC library
//
// Tests for source block partitioning's divisors.
//
// File::encode(), the Content-Encoding path, is deliberately NOT covered here. It runs inside the
// File constructor and the only way to reach it with a usable FEC OTI is through
// Transmitter::FileDescription, whose merge_fec_oti() is protected, so a unit test cannot
// configure one without standing up a Transmitter with its sockets and io_context. That gap is
// recorded rather than worked around; see the commit that fixed the compress loop.

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "File.h"

using namespace LibFlute;

namespace {

FecOti oti_with(uint32_t encoding_symbol_length, uint32_t max_source_block_length) {
  FecOti oti{};
  oti.encoding_id = FecScheme::CompactNoCode;
  oti.transfer_length = 4096;
  oti.encoding_symbol_length = encoding_symbol_length;
  oti.max_source_block_length = max_source_block_length;
  return oti;
}

std::shared_ptr<File> build(const FecOti &oti, std::vector<char> &data) {
  return std::make_shared<File>(/*toi*/1, oti, "http://example.invalid/o",
                                "application/octet-stream", /*expires*/0,
                                data.data(), data.size(), /*copy_data*/true);
}

}  // namespace

/* Both values are used as denominators in RFC 5052 clause 9.1 partitioning. A default-constructed
   FecOti leaves them 0, which made the first division produce inf, the block count inf, and block
   creation effectively unbounded, so the constructor hung instead of reporting anything. The
   public constructors accept a FecOti without inspecting it, so this is reachable by a caller. */

TEST(PartitioningDivisorTest, UsableFecOtiIsAccepted) {
  std::vector<char> data(4096, 'x');
  EXPECT_NO_THROW(build(oti_with(1400, 64), data));
}

TEST(PartitioningDivisorTest, ZeroEncodingSymbolLengthIsRefusedNotHung) {
  std::vector<char> data(4096, 'x');
  EXPECT_THROW(build(oti_with(0, 64), data), std::runtime_error);
}

TEST(PartitioningDivisorTest, ZeroMaxSourceBlockLengthIsRefusedNotHung) {
  std::vector<char> data(4096, 'x');
  EXPECT_THROW(build(oti_with(1400, 0), data), std::runtime_error);
}

TEST(PartitioningDivisorTest, DefaultConstructedFecOtiIsRefused) {
  // The exact shape that hung: nothing configured at all.
  std::vector<char> data(4096, 'x');
  FecOti bare{};
  bare.encoding_id = FecScheme::CompactNoCode;
  bare.transfer_length = 4096;
  EXPECT_THROW(build(bare, data), std::runtime_error);
}
