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
      // Source-specific multicast (SSM, RFC 4607): admits only packets from source_address,
      // as indicated by an SDP a=source-filter line (RFC 4570; TS 26.517 cl.6.2.2.3's own
      // examples use this for FLUTE sessions). boost::asio has no portable SSM join, so this
      // goes straight to the socket options Linux (and most other stacks) actually define --
      // IPv4's ip_mreq_source/IP_ADD_SOURCE_MEMBERSHIP identifies the interface by address;
      // IPv6's group_source_req/MCAST_JOIN_SOURCE_GROUP identifies it by index instead
      // (see resolve_iface_index() above), which is why the two branches build genuinely
      // different structures rather than sharing one.
      if (is_v6) {
        struct group_source_req gsr{};
        gsr.gsr_interface = resolve_iface_index(iface);

        struct sockaddr_in6 grp{};
        grp.sin6_family = AF_INET6;
        auto mcast_bytes = mcast_address.to_v6().to_bytes();
        std::memcpy(&grp.sin6_addr, mcast_bytes.data(), mcast_bytes.size());
        std::memcpy(&gsr.gsr_group, &grp, sizeof(grp));

        struct sockaddr_in6 src{};
        src.sin6_family = AF_INET6;
        auto src_bytes = boost::asio::ip::make_address(source_address).to_v6().to_bytes();
        std::memcpy(&src.sin6_addr, src_bytes.data(), src_bytes.size());
        std::memcpy(&gsr.gsr_source, &src, sizeof(src));

        if (setsockopt(_socket.native_handle(), IPPROTO_IPV6, MCAST_JOIN_SOURCE_GROUP,
                        &gsr, sizeof(gsr)) != 0) {
          spdlog::error("Receiver: MCAST_JOIN_SOURCE_GROUP for {} from {} failed: {}", address,
                        source_address, strerror(errno));
        } else {
          spdlog::info("Receiver: joined SSM {} from source {} on iface {}", address,
                       source_address, iface);
        }
      } else {
        struct ip_mreq_source mreq_source{};
        auto mcast_bytes = mcast_address.to_v4().to_bytes();
        auto src_bytes = boost::asio::ip::make_address(source_address).to_v4().to_bytes();
        auto iface_bytes = boost::asio::ip::make_address(iface).to_v4().to_bytes();
        std::memcpy(&mreq_source.imr_multiaddr, mcast_bytes.data(), mcast_bytes.size());
        std::memcpy(&mreq_source.imr_sourceaddr, src_bytes.data(), src_bytes.size());
        std::memcpy(&mreq_source.imr_interface, iface_bytes.data(), iface_bytes.size());

        if (setsockopt(_socket.native_handle(), IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP,
                        &mreq_source, sizeof(mreq_source)) != 0) {
          spdlog::error("Receiver: IP_ADD_SOURCE_MEMBERSHIP for {} from {} failed: {}", address,
                        source_address, strerror(errno));
        } else {
          spdlog::info("Receiver: joined SSM {} from source {} on iface {}", address,
                       source_address, iface);
        }
      }
    } else if (is_v6) {
      // Plain (any-source) multicast join, IPv6: join_group(address_v6, interface_index) --
      // a different overload shape than IPv4's join_group(address_v4, address_v4) below,
      // since v6 identifies the interface by index (see resolve_iface_index()).
      _socket.set_option(
          boost::asio::ip::multicast::join_group(
            mcast_address.to_v6(), resolve_iface_index(iface)));
    } else {
      // Join the multicast group on the specific interface passed in - the
      // single-address join_group() overload ignores `iface` entirely and
      // joins via whatever interface the system considers the default route
      // for the group, which silently breaks reception when the content
      // actually arrives on a non-default interface (e.g. the modem's own
      // TUN device rather than a physical NIC).
      _socket.set_option(
          boost::asio::ip::multicast::join_group(
            mcast_address.to_v4(),
            boost::asio::ip::make_address(iface).to_v4()));
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

auto LibFlute::Receiver::enable_ipsec(uint32_t spi, const std::string& key, const std::string& auth_key) -> void
{
  LibFlute::IpSec::enable_esp(spi, _mcast_address, LibFlute::IpSec::Direction::In, key, auth_key);
}

auto LibFlute::Receiver::handle_receive_from(const boost::system::error_code& error,
    size_t bytes_recvd) -> void
{
  if (!_running) return;

  if (!error)
  {
    spdlog::trace("Received {} bytes", bytes_recvd);
    try {
      auto alc = LibFlute::AlcPacket(_data, bytes_recvd);

      if (alc.tsi() == _tsi) {

        const std::lock_guard<std::mutex> lock(_files_mutex);

        if (alc.close_session() && !_session_closed) {
          _session_closed = true;
          spdlog::info("Sender signalled Close Session for TSI {}", alc.tsi());
          if (_close_session_cb) _close_session_cb();
        }
        if (alc.close_object() && _closed_object_tois_notified.insert(alc.toi()).second) {
          spdlog::debug("Sender signalled Close Object for TOI {}", alc.toi());
          if (_close_object_cb) _close_object_cb(alc.toi());
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

        if (alc.toi() != 0 && _files.find(alc.toi()) == _files.end() && alc.has_fti()) {
          // No FDT entry (and no File at all) for this TOI yet, but this packet
          // carries its own EXT_FTI (RFC 6726 SS3.4.1 explicitly allows a receiver
          // to bootstrap reception of an object from its own FEC OTI before the
          // FDT describing it has arrived). Previously such packets were simply
          // discarded until the FDT caught up, potentially losing symbols carried
          // by the very packets that would have started reception soonest.
          // content_location/content_type/md5 are unknown until the FDT entry for
          // this TOI arrives; the existing toi==0 handling above already
          // reconciles a stale/incomplete File if the FDT's content_location for
          // this TOI later turns out to differ.
          spdlog::debug("Bootstrapping reception for file with TOI {} from its own EXT_FTI (FDT not yet received)", alc.toi());
          FileDeliveryTable::FileEntry fe{static_cast<uint32_t>(alc.toi()), "", static_cast<uint32_t>(alc.fec_oti().transfer_length), "", "", 0, alc.fec_oti()};
          _files[alc.toi()] = std::make_shared<LibFlute::File>(fe);
        }

        if (_files.find(alc.toi()) != _files.end() && !_files[alc.toi()]->complete()) {
          auto encoding_symbols = LibFlute::EncodingSymbol::from_payload(
              _data + alc.header_length(),
              bytes_recvd - alc.header_length(),
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
              if (it->second.get() != file && it->second->meta().content_location == file->meta().content_location)
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
                if (existing_file != _files.end() &&
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
