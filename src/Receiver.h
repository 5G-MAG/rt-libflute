
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
