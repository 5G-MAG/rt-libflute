// AlcConstants.h
// Header extension constants for ALC/FLUTE
#pragma once

#include <cstdint>

namespace AlcHeaderExtension
{
    constexpr uint8_t EXT_NOP = 0x00;
    constexpr uint8_t EXT_AUTH = 0x01;
    constexpr uint8_t EXT_TIME = 0x02;
    constexpr uint8_t EXT_FTI = 0x20;
    constexpr uint8_t EXT_FDT = 0x40;
    constexpr uint8_t EXT_CENC = 0x50;
}

namespace FileDeliveryTableConstants
{
    enum FdtNamespace
    {
        FDT_NS_NONE = 0,
        FDT_NS_RFC3926,
        FDT_NS_DRAFT_2005,
        FDT_NS_RFC6726, // FLUTE v2 - will need other things implementing to use this correctly
        FDT_NS_3GPP_CONSOLIDATED_V2
    };

    constexpr const char* FDT_NS_URL_RFC3926 = "http://www.example.com/flute";
    constexpr const char* FDT_NS_URL_MBMS_2007 = "urn:3GPP:metadata:2007:MBMS:FLUTE:FDT";
    constexpr const char* FDT_NS_URL_MBMS_2012 = "urn:3GPP:metadata:2012:MBMS:FLUTE:FDT";
    constexpr const char* FDT_NS_URL_DRAFT_2005 = "urn:IETF:metadata:2005:FLUTE:FDT";
    constexpr const char* FDT_NS_URL_3GPP_2022 = "urn:3GPP:metadata:2022:FLUTE:FDT";
}
