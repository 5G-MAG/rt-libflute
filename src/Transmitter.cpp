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
#include <cstdio>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include "spdlog/spdlog.h"
#include "Transmitter.h"
#include "IpSec.h"
LibFlute::Transmitter::Transmitter ( const std::string& address, short port,
uint64_t tsi, unsigned short mtu, uint32_t rate_limit, FecScheme fec_scheme,
boost::asio::io_service& io_service)
    : _endpoint(boost::asio::ip::address::from_string(address), port)
    , _socket(io_service, _endpoint.protocol())
    , _fdt_timer(io_service)
    , _send_timer(io_service)
    , _io_service(io_service)
    , _tsi(tsi)
    , _mtu(mtu)
    , _rate_limit(rate_limit)
    , _mcast_address(address)
    ,_fec_scheme(fec_scheme)
{
  _max_payload = mtu -
    ( _endpoint.address().is_v6() ? 40 : 20) - // IP header
     8 - // UDP header
    32 - // ALC Header with EXT_FDT and EXT_FTI
     4;  // SBN and ESI for compact no-code or raptor FEC
  uint32_t max_source_block_length = 64;

  switch(_fec_scheme) {
    case FecScheme::Raptor:
      if (_max_payload % 4) {
        _max_payload -= (_max_payload % 4); // must be divisible by Al = 4
      }
      break;
    default:
      break;
  }

  _socket.set_option(boost::asio::ip::multicast::enable_loopback(true));
  _socket.set_option(boost::asio::ip::udp::socket::reuse_address(true));

  _fec_oti = FecOti{_fec_scheme, 0, _max_payload, max_source_block_length};
  _fdt = std::make_unique<FileDeliveryTable>(1, _fec_oti);

  _fdt_timer.expires_from_now(boost::posix_time::seconds(_fdt_repeat_interval));
  _fdt_timer.async_wait( boost::bind(&Transmitter::fdt_send_tick, this));

  send_next_packet();
}

LibFlute::Transmitter::~Transmitter() = default;

auto LibFlute::Transmitter::enable_ipsec(uint32_t spi, const std::string& key) -> void 
{
  LibFlute::IpSec::enable_esp(spi, _mcast_address, LibFlute::IpSec::Direction::Out, key);
}

auto LibFlute::Transmitter::handle_send_to(const boost::system::error_code& error) -> void
{
  if (!error) {
  }
}

auto LibFlute::Transmitter::seconds_since_epoch() -> uint64_t 
{
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

auto LibFlute::Transmitter::send_fdt() -> void {
  _fdt->set_expires(seconds_since_epoch() + _fdt_repeat_interval * 2);
  auto fdt = _fdt->to_string();
  auto fdt_fec_oti = _fec_oti;
  fdt_fec_oti.encoding_id = FecScheme::CompactNoCode; // always send the FDT in "plaintext"
  fdt_fec_oti.encoding_symbol_length = _mtu -
    20 - // IPv4 header
     8 - // UDP header
    32 - // ALC Header with EXT_FDT and EXT_FTI
     4;  // SBN and ESI for compact no-code FEC
  fdt_fec_oti.max_source_block_length = 64;
  auto file = std::make_shared<File>(
        0,
        fdt_fec_oti,
        "",
        "",
        seconds_since_epoch() + _fdt_repeat_interval * 2,
        (char*)fdt.c_str(),
        fdt.length(),
        true);
  file->set_fdt_instance_id( _fdt->instance_id() );
  _files.insert_or_assign(0, file);
}

auto LibFlute::Transmitter::send(
    const std::string& content_location,
    const std::string& content_type,
    uint32_t expires,
    char* data,
    size_t length) -> uint16_t 
{
  auto toi = _toi;
  std::shared_ptr<File> file;
  try {
      file = std::make_shared<File>(
          toi,
          _fec_oti,
          content_location,
          content_type,
          expires,
          data,
          length);
  } catch (const char *e) {
    spdlog::error("Failed to create File object for file {} : {}", content_location, e);
    return -1;
  }

  _toi++;
  if (_toi == 0) _toi = 1; // clamp to >= 1 in case it wraps

  _fdt->add(file->meta());
  send_fdt();
  _files.insert({toi, file});
  return toi;
}

auto LibFlute::Transmitter::fdt_send_tick() -> void
{
  send_fdt();
  _fdt_timer.expires_from_now(boost::posix_time::seconds(_fdt_repeat_interval));
  _fdt_timer.async_wait( boost::bind(&Transmitter::fdt_send_tick, this));
}

auto LibFlute::Transmitter::file_transmitted(uint32_t toi) -> void
{
  if (toi != 0) {
    _files.erase(toi);
    _fdt->remove(toi);
    send_fdt();

    if (_completion_cb) {
      _completion_cb(toi);
    }
  }
}

auto LibFlute::Transmitter::send_next_packet() -> void
{
  uint32_t bytes_queued = 0;

  if (_files.size()) {
    for (auto& file_m : _files) {
      auto file = file_m.second;

      if (file && !file->complete()) {
        auto symbols = file->get_next_symbols(_max_payload);

        if (symbols.size()) {
          for(const auto& symbol : symbols) {
            spdlog::debug("sending TOI {} SBN {} ID {}", file->meta().toi, symbol.source_block_number(), symbol.id() );
          }
          auto packet = std::make_shared<AlcPacket>(_tsi, file->meta().toi, file->meta().fec_oti, symbols, _max_payload, file->fdt_instance_id());
          bytes_queued += packet->size();
          spdlog::debug("Queued ALC packet of {} bytes, containing {} symbols, for TOI {} , for transmission", packet->size(), symbols.size(), file->meta().toi );

          _socket.async_send_to(
              boost::asio::buffer(packet->data(), packet->size()), _endpoint,
              [file, symbols, packet, this](
                const boost::system::error_code& error,
                std::size_t bytes_transferred)
              {
                if (error) {
                  spdlog::debug("send_to error: {}", error.message());
                } else {
                  file->mark_completed(symbols, !error);
                  if (file->complete()) {
                    file_transmitted(file->meta().toi);
                  }
                }
              });
        } 
        break;
      }
    }
  } 
  if (!bytes_queued) {
    _send_timer.expires_from_now(boost::posix_time::milliseconds(10));
    _send_timer.async_wait( boost::bind(&Transmitter::send_next_packet, this));
  } else {
    if (_rate_limit == 0) {
      _io_service.post(boost::bind(&Transmitter::send_next_packet, this));
    } else {
      auto send_duration = ((bytes_queued * 8.0) / (double)_rate_limit/1000.0) * 1000.0 * 1000.0;
      spdlog::debug("Rate limiter: queued {} bytes, limit {} kbps, next send in {} us", 
          bytes_queued, _rate_limit, send_duration);
      _send_timer.expires_from_now(boost::posix_time::microseconds(
            static_cast<int>(ceil(send_duration))));
      _send_timer.async_wait( boost::bind(&Transmitter::send_next_packet, this));
    }
  }
}
