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
// Regression coverage for two FDT-growth bugs found and fixed while
// harmonizing divergent branches of this project (see the commit history
// for the full story): repeated carousel sends of an unchanged
// FileDescription used to leave one stale <File> entry per resend in the
// FDT, and a FileDescription whose content genuinely changes used to leave
// its *previous* TOI's entry orphaned forever. Both grew the serialised FDT
// XML without bound over a long-running carousel.
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include "Transmitter.h"

using namespace LibFlute;

namespace {
// Transmitter has no public accessor for the current FDT XML; construct one
// with no tunnel/network side effects that matter for this test (the
// io_context is never run, so nothing actually gets sent) and inspect FDT
// size growth indirectly via repeated identical sends.
size_t fdt_entry_count(const std::string& fdt_xml) {
  size_t count = 0, pos = 0;
  while ((pos = fdt_xml.find("<File ", pos)) != std::string::npos) {
    count++;
    pos += 6;
  }
  return count;
}
} // namespace

TEST(FdtGrowthTest, CarouselResendOfUnchangedContentDoesNotGrowFdt) {
  boost::asio::io_context io;
  Transmitter tx("239.255.9.1", 19001, 9001, 1400, 0, io);

  std::string content = "hello world, unchanged across every resend";
  auto desc = std::make_shared<Transmitter::FileDescription>("carousel/item.txt", content.c_str(), content.size());

  // First send assigns a TOI; every subsequent send reuses it unchanged --
  // exactly the carousel-repeat pattern that used to leak one FDT entry per
  // cycle.
  uint16_t toi1 = tx.send(desc);
  size_t fdt1 = tx.fdt().to_string().size();
  size_t entries1 = fdt_entry_count(tx.fdt().to_string());

  for (int i = 0; i < 20; i++) {
    tx.send(desc);
  }

  uint16_t toi_final = desc->toi();
  EXPECT_EQ(toi1, toi_final) << "unchanged content must keep the same TOI across resends";
  EXPECT_EQ(fdt_entry_count(tx.fdt().to_string()), entries1) << "resending unchanged content must not add FDT entries";
  EXPECT_EQ(tx.fdt().to_string().size(), fdt1) << "FDT XML size must not grow across identical resends";
}

TEST(FdtGrowthTest, ContentChangeRemovesThePreviousToisFdtEntry) {
  boost::asio::io_context io;
  Transmitter tx("239.255.9.2", 19002, 9002, 1400, 0, io);

  std::string content1 = "version one of the content";
  auto desc = std::make_shared<Transmitter::FileDescription>("changing/item.txt", content1.c_str(), content1.size());
  uint16_t toi1 = tx.send(desc);
  size_t entries_after_first = fdt_entry_count(tx.fdt().to_string());

  // Changing the content zeroes the TOI (via _reset_toi(), remembering
  // toi1 as _previous_toi); the next send must assign a fresh TOI AND clean
  // up toi1's now-stale FDT entry, not just add a new one alongside it.
  std::string content2 = "version two -- genuinely different, much longer content than before";
  desc->set_content(content2.c_str(), content2.size());
  uint16_t toi2 = tx.send(desc);

  EXPECT_NE(toi1, toi2) << "changed content must get a fresh TOI";
  EXPECT_EQ(fdt_entry_count(tx.fdt().to_string()), entries_after_first)
      << "the old TOI's FDT entry must be removed, not left orphaned alongside the new one";
}

TEST(FdtGrowthTest, RepeatedContentChangesDoNotAccumulateOrphans) {
  boost::asio::io_context io;
  Transmitter tx("239.255.9.3", 19003, 9003, 1400, 0, io);

  std::string content = "iteration 0";
  auto desc = std::make_shared<Transmitter::FileDescription>("changing/loop.txt", content.c_str(), content.size());
  tx.send(desc);
  size_t entries_after_first = fdt_entry_count(tx.fdt().to_string());

  for (int i = 1; i <= 10; i++) {
    std::string next_content = "iteration " + std::to_string(i) + " with some extra padding to change length too";
    desc->set_content(next_content.c_str(), next_content.size());
    tx.send(desc);
  }

  EXPECT_EQ(fdt_entry_count(tx.fdt().to_string()), entries_after_first)
      << "10 content changes must still leave exactly one FDT entry for this object, not 11";
}
