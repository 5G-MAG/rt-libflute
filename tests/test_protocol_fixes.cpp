#include <gtest/gtest.h>

#include <string>

#include "FileDeliveryTable.h"

using namespace LibFlute;

namespace {

FecOti make_fec_oti() {
  FecOti oti{};
  oti.encoding_id = FecScheme::CompactNoCode;
  oti.instance_id = 0;
  oti.transfer_length = 4096;
  oti.encoding_symbol_length = 1400;
  oti.max_source_block_length = 64;
  oti.max_number_of_encoding_symbols = 0;
  return oti;
}

FileDeliveryTable::FileEntry make_entry(const FecOti &oti) {
  FileDeliveryTable::FileEntry e{};
  e.toi = 1;
  e.content_location = "http://example.invalid/seg1.m4s";
  e.content_length = 4096;
  e.expires = 0;
  e.fec_oti = oti;
  e.cache_control.no_cache = false;
  return e;
}

// Serialise a one-file FDT in the given namespace mode.
std::string emit(FileDeliveryTable::FdtNamespace ns) {
  auto oti = make_fec_oti();
  FileDeliveryTable fdt(1, oti, ns);
  fdt.add(make_entry(oti));
  return fdt.to_string();
}

}  // namespace

/* TS 26.346 V18.2.0 clause L.4.4 lists Transfer-Length among the attributes that
   "shall not be carried in the FDT sent by the FLUTE sender". These assert on the
   emitted XML rather than on the FileEntry, because the object carrying a value the
   serialiser then withholds is exactly the case that has to pass. */

TEST(MbmsDownloadProfileTest, TransferLengthNotCarriedInConsolidatedV2Schema) {
  EXPECT_EQ(emit(FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2).find("Transfer-Length"),
            std::string::npos);
}

TEST(MbmsDownloadProfileTest, TransferLengthNotCarriedInDraft2005Schema) {
  EXPECT_EQ(emit(FileDeliveryTable::FDT_NS_DRAFT_2005).find("Transfer-Length"),
            std::string::npos);
}

TEST(MbmsDownloadProfileTest, TransferLengthStillCarriedOutsideTheProfile) {
  // Not a 3GPP session, so RFC 3926's permission applies and the attribute is kept.
  EXPECT_NE(emit(FileDeliveryTable::FDT_NS_RFC3926).find("Transfer-Length"),
            std::string::npos);
}

TEST(MbmsDownloadProfileTest, ContentLengthIsCarriedInEveryMode) {
  // Guards the fallback the prohibition relies on: with Transfer-Length withheld, a
  // receiver derives the transfer length from Content-Length.
  for (auto ns : {FileDeliveryTable::FDT_NS_3GPP_CONSOLIDATED_V2,
                  FileDeliveryTable::FDT_NS_DRAFT_2005,
                  FileDeliveryTable::FDT_NS_RFC3926}) {
    EXPECT_NE(emit(ns).find("Content-Length"), std::string::npos);
  }
}
