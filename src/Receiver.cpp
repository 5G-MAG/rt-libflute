// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
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
#include <chrono>
#include "Receiver.h"
#include "AlcPacket.h"
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include "spdlog/spdlog.h"
#include "IpSec.h"

namespace {
  // IPv6 multicast join (both plain join_group() and MCAST_JOIN_SOURCE_GROUP)
  // identifies the local interface by OS index, unlike IPv4's by-address
  // ip_mreq[_source]/join_group(v4,v4) -- so restoring v6 support alongside
  // the v4-specific-interface-join fix below needs a way to turn the same
  // `iface` address string callers already pass into an interface index.
  // Returns 0 (the kernel's "let it choose" value) if iface_address is
  // empty/unspecified for its family or doesn't match any local interface.
  unsigned int resolve_iface_index(const std::string& iface_address) {
    if (iface_address.empty() || iface_address == "0.0.0.0" || iface_address == "::") {
      return 0;
    }
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
      return 0;
    }
    unsigned int result = 0;
    for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr) continue;
      char host[INET6_ADDRSTRLEN] = {};
      if (ifa->ifa_addr->sa_family == AF_INET6) {
        auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, host, sizeof(host)) && iface_address == host) {
          result = if_nametoindex(ifa->ifa_name);
          break;
        }
      } else if (ifa->ifa_addr->sa_family == AF_INET) {
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)) && iface_address == host) {
          result = if_nametoindex(ifa->ifa_name);
          break;
        }
      }
    }
    freeifaddrs(ifaddr);
    return result;
  }
}

LibFlute::Receiver::Receiver ( const std::string& iface, const std::string& address,
    short port, uint64_t tsi,
    boost::asio::io_context& io_context,
    const std::string& source_address)
    : _socket(io_context)
    , _tsi(tsi)
    , _mcast_address(address)
    , _mcast_port(static_cast<unsigned short>(port))
{
    // Restored alongside the ANY-bind/specific-interface-join fixes below:
    // an earlier version of those fixes made this whole constructor IPv4
    // only, where the original code let Boost pick v4 vs v6 based on the
    // address types passed in. `address`'s family (not `iface`'s -- iface
    // can legitimately be "0.0.0.0"/"::"/empty, meaning "any") is the
    // authoritative signal for which family this session actually uses.
    auto mcast_address = boost::asio::ip::make_address(address);
    bool is_v6 = mcast_address.is_v6();

    // Bind to ANY, not the specific interface address: incoming multicast
    // packets are addressed to the group, not to a particular unicast
    // interface address, so binding to that unicast address is non-standard
    // - and in practice it also breaks epoll-based readiness notification
    // for this socket (confirmed directly: async_receive_from never
    // completes when bound to a specific interface address, even though the
    // data really does arrive and a plain synchronous recv() picks it up;
    // binding to ANY fixes it). `iface` is still used below to select which
    // interface's multicast membership to join.
    boost::asio::ip::udp::endpoint listen_endpoint(
        is_v6 ? boost::asio::ip::address(boost::asio::ip::address_v6::any())
              : boost::asio::ip::address(boost::asio::ip::address_v4::any()),
        port);
    _socket.open(listen_endpoint.protocol());
    _socket.set_option(boost::asio::ip::multicast::enable_loopback(true));
    _socket.set_option(boost::asio::ip::udp::socket::reuse_address(true));
    _socket.set_option(boost::asio::socket_base::receive_buffer_size(16*1024*1024));
    _socket.bind(listen_endpoint);

    if (!source_address.empty()) {
      _expected_source = boost::asio::ip::make_address(source_address);
    }

    _iface = iface;
    _ssm_source = source_address;
    if (set_group_membership(mcast_address, /*join*/ true)) {
      _joined_groups.insert(mcast_address.to_string());
    }

    arm_receive();
}

LibFlute::Receiver::~Receiver()
{
  *_alive = false;
}

auto LibFlute::Receiver::arm_receive() -> void
{
  auto alive = _alive;
  _socket.async_receive_from(
      boost::asio::buffer(_data, max_length), _sender_endpoint,
      [this, alive](const boost::system::error_code& error, size_t bytes_recvd) {
        if (!*alive) return;
        handle_receive_from(error, bytes_recvd);
      });
}

namespace {
  /** Current time on the NTP epoch, matching the base the FDT's Expires attribute uses. */
  auto ntp_seconds_now() -> uint64_t
  {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 2'208'988'800;
  }
}


auto LibFlute::Receiver::set_group_membership(const boost::asio::ip::address& group, bool join) -> bool
{
  const bool is_v6 = group.is_v6();
  const char* verb = join ? "join" : "leave";

  if (!_ssm_source.empty()) {
    /* Source-specific multicast (RFC 4607). boost::asio has no portable SSM call, so this uses the
       socket options the stacks define: IPv4's ip_mreq_source identifies the interface by address,
       IPv6's group_source_req by index, which is why the two build different structures. */
    if (is_v6) {
      struct group_source_req gsr{};
      gsr.gsr_interface = resolve_iface_index(_iface);

      struct sockaddr_in6 grp{};
      grp.sin6_family = AF_INET6;
      auto mcast_bytes = group.to_v6().to_bytes();
      std::memcpy(&grp.sin6_addr, mcast_bytes.data(), mcast_bytes.size());
      std::memcpy(&gsr.gsr_group, &grp, sizeof(grp));

      struct sockaddr_in6 src{};
      src.sin6_family = AF_INET6;
      auto src_bytes = boost::asio::ip::make_address(_ssm_source).to_v6().to_bytes();
      std::memcpy(&src.sin6_addr, src_bytes.data(), src_bytes.size());
      std::memcpy(&gsr.gsr_source, &src, sizeof(src));

      const int opt = join ? MCAST_JOIN_SOURCE_GROUP : MCAST_LEAVE_SOURCE_GROUP;
      if (setsockopt(_socket.native_handle(), IPPROTO_IPV6, opt, &gsr, sizeof(gsr)) != 0) {
        spdlog::error("Receiver: failed to {} SSM {} from {}: {}", verb, group.to_string(),
                      _ssm_source, strerror(errno));
        return false;
      }
    } else {
      struct ip_mreq_source mreq_source{};
      auto mcast_bytes = group.to_v4().to_bytes();
      auto src_bytes = boost::asio::ip::make_address(_ssm_source).to_v4().to_bytes();
      auto iface_bytes = boost::asio::ip::make_address(_iface).to_v4().to_bytes();
      std::memcpy(&mreq_source.imr_multiaddr, mcast_bytes.data(), mcast_bytes.size());
      std::memcpy(&mreq_source.imr_sourceaddr, src_bytes.data(), src_bytes.size());
      std::memcpy(&mreq_source.imr_interface, iface_bytes.data(), iface_bytes.size());

      const int opt = join ? IP_ADD_SOURCE_MEMBERSHIP : IP_DROP_SOURCE_MEMBERSHIP;
      if (setsockopt(_socket.native_handle(), IPPROTO_IP, opt, &mreq_source,
                     sizeof(mreq_source)) != 0) {
        spdlog::error("Receiver: failed to {} SSM {} from {}: {}", verb, group.to_string(),
                      _ssm_source, strerror(errno));
        return false;
      }
    }
    spdlog::info("Receiver: {}ed SSM {} from source {} on iface {}", verb, group.to_string(),
                 _ssm_source, _iface);
    return true;
  }

  /* Any-source multicast. The interface is named explicitly in both families: the single-argument
     join_group() overload ignores it and uses whatever the system considers the default route for
     the group, which silently breaks reception when the traffic arrives elsewhere. IPv6 names the
     interface by index, IPv4 by address. */
  try {
    if (is_v6) {
      const auto idx = resolve_iface_index(_iface);
      if (join) _socket.set_option(boost::asio::ip::multicast::join_group(group.to_v6(), idx));
      else      _socket.set_option(boost::asio::ip::multicast::leave_group(group.to_v6(), idx));
    } else {
      const auto iface_v4 = boost::asio::ip::make_address(_iface).to_v4();
      if (join) _socket.set_option(boost::asio::ip::multicast::join_group(group.to_v4(), iface_v4));
      else      _socket.set_option(boost::asio::ip::multicast::leave_group(group.to_v4(), iface_v4));
    }
  } catch (const std::exception& e) {
    spdlog::error("Receiver: failed to {} group {}: {}", verb, group.to_string(), e.what());
    return false;
  }
  spdlog::info("Receiver: {}ed group {} on iface {}", verb, group.to_string(), _iface);
  return true;
}

auto LibFlute::Receiver::join_channel(const std::string& group) -> bool
{
  if (_joined_groups.count(group)) return false;
  if (!set_group_membership(boost::asio::ip::make_address(group), /*join*/ true)) return false;
  _joined_groups.insert(group);
  return true;
}

auto LibFlute::Receiver::leave_channel(const std::string& group) -> bool
{
  if (!_joined_groups.count(group)) return false;
  if (!set_group_membership(boost::asio::ip::make_address(group), /*join*/ false)) return false;
  _joined_groups.erase(group);
  return true;
}

auto LibFlute::Receiver::enable_ipsec(uint32_t spi, const std::string& key, const std::string& auth_key) -> void
{
  LibFlute::IpSec::enable_esp(spi, _mcast_address, _mcast_port, LibFlute::IpSec::Direction::In,
                              key, auth_key);
}

auto LibFlute::Receiver::handle_receive_from(const boost::system::error_code& error,
    size_t bytes_recvd) -> void
{
  if (!_running) return;

  if (!error)
  {
    spdlog::trace("Received {} bytes", bytes_recvd);
    /* The source is checked before the packet is parsed, so traffic that is not this session's
       cannot reach the parser at all, let alone influence how a parse failure is handled.

       RFC 3450 clause 4.5 orders the receiver's steps and puts this one before any processing of the
       payload: "The receiver MUST verify that the sender IP address together with the TSI carried in
       the header matches one of the (sender IP address, TSI) pairs that was received in a Session
       Description and that the receiver is currently joined to."

       Only checkable where the caller named the source. A source-specific join already has the
       kernel filtering on it, so this is defence in depth there against a routing or membership
       mistake; for an any-source session the library has no source to compare against and the
       obligation cannot be met, which is recorded as a limitation rather than passed over. */
    if (_expected_source && _sender_endpoint.address() != *_expected_source) {
      spdlog::warn("Discarding packet from {}, which is not this session's source {}",
                   _sender_endpoint.address().to_string(), _expected_source->to_string());
      arm_receive();
      return;
    }

    try {
      auto alc = LibFlute::AlcPacket(_data, bytes_recvd);

      if (alc.tsi() == _tsi) {

        if (_close_cb && (alc.close_session_flag() || alc.close_object_flag())) {
          _close_cb(alc.close_session_flag(), alc.close_object_flag(), alc.toi());
        }

        /* A packet may legitimately carry nothing, and one that does carries no FEC Payload ID
           either, so there is nothing here to reassemble.

           RFC 3450 clause 4.1: "In some special cases an ALC sender may need to produce ALC
           packets that do not contain any payload."
           The same clause says how to tell: "The total datagram length, conveyed by outer protocol
           headers (e.g., the IP or UDP header), enables receivers to detect the absence of the ALC
           payload and FEC Payload ID."

           FLUTE gives this shape a specific meaning and a specific header.
           RFC 3926 clause 3.1: "the exception that ALC packets sent in a FLUTE session with the
           Close Session (A) flag set to 1 (signaling the end of the session) and that contain no
           payload (carrying no information for any file or FDT) SHALL NOT carry the TOI"

           Falling through was not merely useless. With no TOI the decoded value is zero, so such a
           packet was taken for an FDT packet and restarted FDT reassembly, discarding the instance
           in progress; then the payload walk subtracted a four-byte FEC Payload ID from a length of
           zero, wrapped, and read far past the buffer. One datagram from a conformant peer ending
           its session, and RFC 3926 clause 3.1 is what makes such a peer send one. */
        const size_t payload_len = bytes_recvd - alc.header_length();
        if (payload_len == 0) {
          arm_receive();
          return;
        }

        /* Anything shorter than a FEC Payload ID is not a valid packet, and step 1 of the receiver
           procedure disposes of it before the payload is touched.
           RFC 3450 clause 4.5: "The receiver MUST parse the packet header and verify that it is a
           valid header.  If it is not valid then the packet MUST be discarded without further
           processing." */
        if (payload_len < 4) {
          spdlog::warn("Discarding a {}-byte payload, too short to hold a FEC Payload ID",
                       payload_len);
          arm_receive();
          return;
        }

        const std::lock_guard<std::mutex> lock(_files_mutex);

        /* An expired FDT Instance may not be used to interpret anything that arrives after it.
           TS 26.346 V18.2.0 clause 7.2.9: "For MBMS operation, the UE shall not use a received FDT
           Instance to interpret packets received beyond the expiration time of the FDT Instance."
           The same clause records that this is stricter than RFC 3926, which says only that the
           receiver SHOULD NOT, so the held instance is dropped here and packets for TOIs it
           described stop being interpreted until a fresh instance arrives. Reception of the next
           FDT itself, on TOI 0, is unaffected. */
        if (_fdt && _fdt->expired(ntp_seconds_now())) {
          spdlog::debug("Discarding FDT instance {}, expired at {}", _fdt->instance_id(),
                        _fdt->expires());
          _fdt.reset();
        }

        if (alc.toi() == 0 && (!_fdt || _fdt->instance_id() != alc.fdt_instance_id())) {
          // (Re)start reception of the FDT (TOI 0) for THIS instance. The FDT is
          // a FLUTE object reassembled from its symbols like any file, but unlike
          // a static file its instance changes over the session's lifetime (here
          // every few seconds, as content segments roll out of the live window,
          // each new FDT instance carrying a different file set). If we kept
          // feeding symbols from a newer instance into the File started for an
          // older one, the two serialisations would overlay into one buffer and
          // corrupt it (e.g. a dropped byte at the seam, so an attribute like
          // Content-Location loses its '='). So whenever the in-progress TOI-0
          // object belongs to a different instance than the arriving packet,
          // discard it and reassemble the new instance from scratch.
          auto existing = _files.find(0);
          if (existing == _files.end() || _fdt_in_progress_instance_id != alc.fdt_instance_id()) {
            FileDeliveryTable::FileEntry fe{0, "", static_cast<uint32_t>(alc.fec_oti().transfer_length), "", "", 0, alc.fec_oti()};
            _files[0] = std::make_shared<LibFlute::File>(fe);
            _fdt_in_progress_instance_id = alc.fdt_instance_id();
          }
        }

        if (alc.toi() != 0 && _files.find(alc.toi()) == _files.end() && alc.has_fec_oti()) {
          // No <File> entry for this TOI yet (the FDT describing it hasn't arrived, or won't --
          // RFC 3926 clause 5 makes EXT_FTI support mandatory for a receiver on any TOI other
          // reception doesn't have to wait on that). Bootstrap the FEC OTI straight from this
          // packet instead of discarding it; content_location is filled in later, either from
          // the FDT once it arrives (see the merge below) or left blank if it never does.
          FileDeliveryTable::FileEntry fe{static_cast<uint32_t>(alc.toi()), "", static_cast<uint32_t>(alc.fec_oti().transfer_length), "", "", 0, alc.fec_oti()};
          _files[alc.toi()] = std::make_shared<LibFlute::File>(fe);
        }

        if (_files.find(alc.toi()) != _files.end() && !_files[alc.toi()]->complete()) {
          auto encoding_symbols = LibFlute::EncodingSymbol::from_payload(
              _data + alc.header_length(),
              payload_len,
              _files[alc.toi()]->fec_oti(),
              alc.content_encoding());

          for (const auto& symbol : encoding_symbols) {
            spdlog::debug("received TOI {} SBN {} ID {}", alc.toi(), symbol.source_block_number(), symbol.id() );
            _files[alc.toi()]->put_symbol(symbol);
          }

          auto file = _files[alc.toi()].get();
          if (_files[alc.toi()]->complete()) {
            for (auto it = _files.cbegin(); it != _files.cend();)
            {
              // An empty content location is not an identifying URL. It is what the TOI 0 FDT's
              // own transient file carries, and what a file bootstrapped from a packet's own
              // EXT_FTI carries until its FDT entry arrives. Matching on it would erase an
              // unrelated bootstrapped file the moment the FDT completed, which is every time.
              if (it->second.get() != file && !file->meta().content_location.empty() &&
                  it->second->meta().content_location == file->meta().content_location)
              {
                spdlog::debug("Replacing file with TOI {}", it->first);
                it = _files.erase(it);
              }
              else
              {
                ++it;
              }
            }

            file->decode();

            spdlog::debug("File with TOI {} completed", alc.toi());
            if (alc.toi() != 0 && _completion_cb) {
              _completion_cb(_files[alc.toi()]);
              _files.erase(alc.toi());
            }

            if (alc.toi() == 0) { // parse complete FDT
              _fdt = std::make_unique<LibFlute::FileDeliveryTable>(
                  alc.fdt_instance_id(), _files[alc.toi()]->buffer(), _files[alc.toi()]->length());

              _files.erase(alc.toi());
              for (const auto& file_entry : _fdt->file_entries()) {
                // automatically receive all files in the FDT
                auto existing_file = _files.find(file_entry.toi);
                if (existing_file != _files.end() && existing_file->second->meta().content_location.empty() &&
                    !existing_file->second->complete()) {
                  // Reception for this TOI was bootstrapped from a packet's own EXT_FTI before
                  // this FDT arrived (content_location wasn't known yet) -- this is that same
                  // in-progress transfer, not a stale one. Adopt the FDT's metadata in place
                  // rather than discarding and restarting it.
                  existing_file->second->adopt_fdt_metadata(file_entry);
                } else if (existing_file != _files.end() &&
                    existing_file->second->meta().content_location != file_entry.content_location) {
                  // TOI numbers get reused across FDT instances (the live window
                  // rolls forward). If a File is still sitting here incomplete
                  // under this TOI from an earlier instance and this instance
                  // now describes a *different* content_location for the same
                  // TOI, that object is stale, abandoned reception state, not
                  // an in-progress transfer of the current file. Feeding this
                  // file's symbols into it would corrupt the buffer (mismatched
                  // source-block layout) and its received_at would keep the
                  // timestamp from the abandoned transfer, making the completed
                  // file look ancient to cache expiry the instant it lands. Drop
                  // it and start clean.
                  spdlog::debug("Discarding stale incomplete file for reused TOI {} ({} != {})",
                      file_entry.toi, existing_file->second->meta().content_location, file_entry.content_location);
                  _files.erase(existing_file);
                  existing_file = _files.end();
                }
                if (existing_file == _files.end()) {
                  spdlog::debug("Starting reception for file with TOI {}: {} ({})", file_entry.toi,
                      file_entry.content_location, file_entry.content_type);
                  _files.emplace(file_entry.toi, std::make_shared<LibFlute::File>(file_entry));
                }
              }
            }
          }
        } else {
          spdlog::trace("Discarding packet for unknown or already completed file with TOI {}", alc.toi());
        }
      } else {
        spdlog::warn("Discarding packet for unknown TSI {}", alc.tsi());
      }
    } catch (const std::exception &ex) {
      spdlog::warn("Failed to decode ALC/FLUTE packet: {}", ex.what());
    } catch (const char* ex) {
      // AlcPacket/EncodingSymbol/File/FileDeliveryTable all throw raw
      // string literals (not std::exception) for malformed/unsupported
      // packets - a single such packet would otherwise propagate past this
      // handler entirely uncaught and crash the whole receiver via
      // std::terminate().
      spdlog::warn("Failed to decode ALC/FLUTE packet: {}", ex);
    }

    arm_receive();
  }
  else
  {
    spdlog::error("receive_from error: {}", error.message());
  }
}

auto LibFlute::Receiver::file_list() -> std::vector<std::shared_ptr<LibFlute::File>>
{
  std::vector<std::shared_ptr<LibFlute::File>> files;
  for (auto& f : _files) {
    files.push_back(f.second);
  }
  return files;
}

auto LibFlute::Receiver::remove_expired_files(unsigned max_age) -> void
{
  const std::lock_guard<std::mutex> lock(_files_mutex);
  for (auto it = _files.cbegin(); it != _files.cend();)
  {
    auto age = time(nullptr) - it->second->received_at();
    if ( it->second->meta().content_location != "bootstrap.multipart"  && age > max_age) {
      it = _files.erase(it);
    } else {
      ++it;
    }
  }
}

auto LibFlute::Receiver::remove_file_with_content_location(const std::string& cl) -> void
{
  const std::lock_guard<std::mutex> lock(_files_mutex);
  for (auto it = _files.cbegin(); it != _files.cend();)
  {
    if ( it->second->meta().content_location == cl) {
      it = _files.erase(it);
    } else {
      ++it;
    }
  }
}
