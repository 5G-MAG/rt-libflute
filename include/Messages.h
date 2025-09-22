#pragma once

namespace Messages {
    // Error messages
    constexpr const char* PACKET_TOO_SHORT = "Packet too short";
    constexpr const char* UNSUPPORTED_LCT_VERSION = "Unsupported LCT version";
    constexpr const char* UNSUPPORTED_CCI_FIELD_LENGTH = "Unsupported CCI field length";
    constexpr const char* TSI_FIELD_NOT_PRESENT = "TSI field not present";
    constexpr const char* TOI_FIELD_NOT_PRESENT = "TOI field not present";
    constexpr const char* TOI_OVER_64_BITS = "TOI fields over 64 bits in length are not supported";
    constexpr const char* ONLY_COMPACT_NO_CODE_FEC = "Only Compact No-Code FEC is supported";
    constexpr const char* INVALID_EXT_FTI_LENGTH = "Invalid length for EXT_FTI header extension";
    constexpr const char* UNSUPPORTED_FLUTE_VERSION = "Unsupported FLUTE version";
    constexpr const char* ROOT_NOT_FDT_INSTANCE = "Root element is not FDT-Instance";
    constexpr const char* FDT_NAMESPACE_NOT_RECOGNISED = "FDT namespace not recognised";
    constexpr const char* MISSING_TOI_ATTRIBUTE = "Missing TOI attribute on File element";
    constexpr const char* MISSING_CONTENT_LOCATION_ATTRIBUTE = "Missing Content-Location attribute on File element";
    constexpr const char* KEY_TOO_LONG = "Key is too long";
    constexpr const char* FAILED_TO_ALLOCATE_FILE_BUFFER = "Failed to allocate file buffer";
    constexpr const char* NO_DATA_ALLOCATED = "No data allocated";
    constexpr const char* UNSUPPORTED_FEC_SCHEME = "Unsupported FEC scheme";
    constexpr const char* ERROR_COMPRESSING_FILE = "Error compressing file {}: {}";
    constexpr const char* UNKNOWN_CONTENT_ENCODING = "Unknown Content-Encoding {}";
    constexpr const char* DECOMPRESSED_LENGTH_MISMATCH = "Decompressed length does not match expected Content-Length ({} != {})";
    constexpr const char* ERROR_DECOMPRESSING_FILE = "Error decompressing file {}: {}";
    constexpr const char* RECEIVE_FROM_ERROR = "receive_from error: {}";

    // Warning messages
    constexpr const char* DISCARD_UNKNOWN_TSI = "Discarding packet for unknown TSI {}";
    constexpr const char* FAILED_DECODE_PACKET = "Failed to decode ALC/FLUTE packet: {}";

    // Trace messages
    constexpr const char* RATE_LIMITER = "Rate limiter: queued {} bytes, limit {} kbps, next send in {} us";
    constexpr const char* RECEIVED_BYTES = "Received {} bytes";
    constexpr const char* DISCARD_UNKNOWN_OR_COMPLETED_FILE = "Discarding packet for unknown or already completed file with TOI {}";

    // Debug messages
    constexpr const char* SENDING_FDT_INSTANCE = "Sending FDT instance {}:\n{}";
    constexpr const char* RESET_TOI_FILE_DESCRIPTION = "Reset TOI for FileDescription";
    constexpr const char* ASSIGNED_NEW_TOI = "Assigned new TOI {}";
    constexpr const char* SENDING_SYMBOL = "sending TOI {} SBN {} ID {}";
    constexpr const char* SENT_TO_ERROR = "sent_to error: {}";
    constexpr const char* RECEIVED_NEW_FDT = "Received new FDT with instance ID {}: {}";
    constexpr const char* RECEIVED_SYMBOL = "received TOI {} SBN {} ID {}";
    constexpr const char* REPLACING_FILE = "Replacing file with TOI {}";
    constexpr const char* FILE_COMPLETED = "File with TOI {} completed";
    constexpr const char* STARTING_RECEPTION = "Starting reception for file with TOI {}: {} ({})";
    constexpr const char* CREATING_FILE_FROM_ENTRY = "Creating File from FileEntry";
    constexpr const char* ALLOCATING_BUFFER = "Allocating buffer";
    constexpr const char* CREATING_FILE_FROM_DESCRIPTION = "Creating File from FileDescription";
    constexpr const char* CREATING_FILE_FROM_DATA = "Creating File from data";
    constexpr const char* DESTROYING_FILE = "Destroying File";
    constexpr const char* FREEING_BUFFER = "Freeing buffer";
    constexpr const char* MD5_MISMATCH = "MD5 mismatch for TOI {}, discarding";
    constexpr const char* COMPRESSING_CONTENTS = "Compressing contents with {}";
    constexpr const char* PART_COMPRESSED = "Part compressed: {} bytes";
}

