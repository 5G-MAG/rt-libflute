// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
// 
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <cstdio>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include "spdlog/spdlog.h"
#include "Transmitter.h"


LibFlute::Transmitter::Transmitter ( const std::string& address,
    short port, uint64_t tsi, unsigned short mtu,
    boost::asio::io_service& io_service)
    : _endpoint(boost::asio::ip::address::from_string(address), port)
    , _socket(io_service, _endpoint.protocol())
    , _fdt_timer(io_service)
    , _send_timer(io_service)
    , _io_service(io_service)
    , _tsi(tsi)
    , _mtu(mtu)
{
  _max_payload = mtu -
    20 - // IPv4 header
     8 - // UDP header
    32 - // ALC Header with EXT_FDT and EXT_FTI
     4;  // SBN and ESI for compact no-code FEC
  uint32_t max_source_block_length = 64;

  _fec_oti = FecOti{FecScheme::CompactNoCode, 0, _max_payload, max_source_block_length};
  _fdt = std::make_unique<FileDeliveryTable>(1, _fec_oti);

  _fdt_timer.expires_from_now(boost::posix_time::seconds(_fdt_repeat_interval));
  _fdt_timer.async_wait( boost::bind(&Transmitter::fdt_send_tick, this));

  send_next_packet();
}

LibFlute::Transmitter::~Transmitter() = default;

auto LibFlute::Transmitter::handle_send_to(const boost::system::error_code& error) -> void
{
  if (!error) {
  }
}

auto LibFlute::Transmitter::seconds_since_epoch() -> uint64_t 
{
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count() +
      2208988800; // use NTP epoch (1.1.1900 00:00:00)
}

auto LibFlute::Transmitter::send_fdt() -> void {
  _fdt->set_expires(seconds_since_epoch() + _fdt_repeat_interval * 2);
  auto fdt = _fdt->to_string();
  auto file = std::make_shared<File>(
        0,
        _fec_oti,
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
    size_t length) -> uint64_t 
{
  auto toi = _toi++;
  if (_toi == 0) _toi = 1; // clamp to >= 1 in case it wraps

  auto file = std::make_shared<File>(
        toi,
        _fec_oti,
        content_location,
        content_type,
        expires,
        data,
        length);

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
  bool packet_queued = false;
  if (_files.size()) {
    for (auto& file_m : _files) {
      auto file = file_m.second;

      if (file && !file->complete()) {
        auto symbols = file->get_next_symbols(_max_payload);

        if (symbols.size()) {
          auto packet = std::make_shared<AlcPacket>(_tsi, file->meta().toi, file->meta().fec_oti, symbols, _max_payload, file->fdt_instance_id());
          packet_queued = true;

          _socket.async_send_to(
              boost::asio::buffer(packet->data(), packet->size()), _endpoint,
              [file, symbols, packet, this](
                const boost::system::error_code& error,
                std::size_t bytes_transferred)
              {
                file->mark_completed(symbols, !error);
                if (file->complete()) {
                  file_transmitted(file->meta().toi);
                }
                _io_service.post(boost::bind(&Transmitter::send_next_packet, this));
              });
        } 
        break;
      }
    }
  } 
  if (!packet_queued) {
    _send_timer.expires_from_now(boost::posix_time::milliseconds(10));
    _send_timer.async_wait( boost::bind(&Transmitter::send_next_packet, this));
  }
}
