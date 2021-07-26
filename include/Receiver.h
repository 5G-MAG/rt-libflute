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

#pragma once
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <string>
#include <map>
#include <mutex>
#include "File.h"
#include "FileDeliveryTable.h"

namespace LibFlute {
  class Receiver {
    public:
      Receiver( const std::string& iface, const std::string& address, 
          short port, uint64_t tsi,
          boost::asio::io_service& io_service);

      virtual ~Receiver();

      std::vector<std::shared_ptr<LibFlute::File>> file_list();
      void remove_expired_files(unsigned max_age);
    private:

      void handle_receive_from(const boost::system::error_code& error,
          size_t bytes_recvd);
      boost::asio::ip::udp::socket _socket;
      boost::asio::ip::udp::endpoint _sender_endpoint;

      enum { max_length = 2048 };
      char _data[max_length];
      uint64_t _tsi;
      std::unique_ptr<LibFlute::FileDeliveryTable> _fdt;
      std::map<uint64_t, std::shared_ptr<LibFlute::File>> _files;
      std::mutex _files_mutex;
  };
};
