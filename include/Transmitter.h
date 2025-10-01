// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//               2025 British Broadcasting Corporation (David Waring <david.waring2@bbc.co.uk>)
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
#pragma once
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <chrono>
#include <queue>
#include <string>
#include <map>
#include <mutex>
#include <optional>
//#include "File.h"
#include "AlcPacket.h"
#include "FileDeliveryTable.h"

namespace LibFlute {

  class File;

  /**
   *  FLUTE transmitter class. Construct an instance of this to send data through a FLUTE/ALC session.
   */
  class Transmitter {
    public:
     /**
      * FDT namespace enumeration
      */
      using FdtNamespace = FileDeliveryTable::FdtNamespace;


     /**
      * File Description object
      */
      class FileDescription {
      public:
        using date_time_type = std::chrono::system_clock::time_point;

        enum CompressionAlgorithm {
          COMPRESSION_NONE = 0,  //< No compression
          COMPRESSION_GZIP,      //< Use gzip compression when encoding the file
          COMPRESSION_DEFLATE    //< Use deflate compression when encoding the file
        };

        FileDescription() = delete;
       /**
        * Make a file description using the contents of a local file
        *
        * @param content_location The URL to use as the content location in the FDT when sending the file
        * @param filename The filename in the local filesystem of the file contents associated with this file description
        */
        FileDescription(const std::string &content_location, const std::string &filename);

       /**@{*/
       /**
        * Make a file description using the contents of a vector
        *
        * This does not copy the data, it only retains a reference to it. It is up to the application to ensure that the
        * data is retained in memory until it has finished with this file description and the Transmitter has finished sending
        * the file.
        *
        * @param content_location The URL to use as the content location in the FDT when sending the file
        * @param data The vector containing the file contents
        */
        FileDescription(const std::string &content_location, const std::vector<char> &data);
        FileDescription(const std::string &content_location, const std::vector<unsigned char> &data);
       /**@}*/

       /**
        * Make a file description using the contents of a memory buffer
        *
        * This does not copy the data, it only retains a reference to it. It is up to the application to ensure that the
        * data is retained in memory until it has finished with this file description and the Transmitter has finished sending
        * the file.
        *
        * @param content_location The URL to use as the content location in the FDT when sending the file
        * @param data A pointer to the memory buffer containing the file contents
        * @param length The size of the file contents in the memory buffer in bytes
        */
        FileDescription(const std::string &content_location, const char *data, size_t length);

       /**
        * Make a file description without contents
        *
        * Create a file description with just a URL location and no body content. The content can be added later using the
        * set_content() methods.
        *
        * @param content_location The URL to use as the content location in the FDT when sending the file
        * @see set_content()
        */
        FileDescription(const std::string &content_location);

       /**
        * Copy constructor
        *
        * This will make a copy of the file description.
        *
        * @param other The other file description to copy
        */
        FileDescription(const FileDescription &other);

       /**
        * Move constructor
        *
        * This will move the resources of the other file description into a new file description.
        *
        * @param other The other file description to move
        */
        FileDescription(FileDescription &&other);

       /**
        * Destructor
        *
        * Free all resources associated with this file description.
        */
        virtual ~FileDescription();

       /**
        * Copy operator
        *
        * This will make a copy of the other file description into this one.
        *
        * @param other The other file description to copy
        *
        * @return this file description
        */
        FileDescription &operator=(const FileDescription &other);

       /**
        * Move operator
        *
        * This will move the resources of the other file description into this one.
        *
        * @param other The other file description to move into this
        *
        * @return this file description
        */
        FileDescription &operator=(FileDescription &&other);

       /**
        * Equality operator
        *
        * Check if two file descriptions are equivalent
        */
        bool operator==(const FileDescription &other) const;

       /**
        * Has a Transmitter associated a TSI with this file?
        *
        * @return `true` if the TSI has been set
        */
        bool has_tsi() const { return _tsi.has_value(); };

       /**
        * Get the associated TSI value
        *
        * @return the TSI value associated with this file description or 0 if not set
        * @see has_tsi()
        */
        uint64_t tsi() const { return _tsi?_tsi.value():0; };

       /**
        * Get the TOI associated with this file description
        *
        * This is meaningless if has_tsi() is false.
        *
        * @return the TOI associated with this file description
        */
        uint32_t toi() const { return _file_entry.toi; };

       /**
        * Get the FDT file entry
        *
        * @return The current FDT File entry
        */
        const FileDeliveryTable::FileEntry &file_entry() const {
          return _file_entry;
        };

       /**
        * Get the data to be transmitted
        *
        * This will apply the compression standard currently set to the contents and provide a transfer buffer.
        *
        * @return The transfer data buffer pointer for the file contents. Use data_length() to get the transfer buffer size
        * @see set_compression()
        * @see data_length()
        */
        const char *data();

       /**
        * Get the length in bytes of the data to be transmitted
        *
        * This will apply the compression standard currently set to the contents and provide the resulting transfer buffer length.
        *
        * @return The transfer data buffer length in bytes. Use data() to get the transfer buffer pointer
        * @see set_compression()
        * @see data()
        */
        size_t data_length();

       /**
        * Set the compression algorithm
        *
        * This sets the compression algorithm that will be used to compress the file contents before sending.
        * This will reset the TOI if the compression algorithm is changed.
        *
        * @param compression The compression algorithm to set
        * @return this file description
        */
        FileDescription &set_compression(CompressionAlgorithm compression);

       /**
        * Set Content-Location
        *
        * @param location The location URL or filename.
        */
        FileDescription &set_content_location(const std::string &location);

       /**
        * Change the file contents using a local file
        *
        * This will alter the contents associated with this file description and replace them with the contents of the local file.
        * This will reset the TOI if the contents have changed.
        *
        * @param filename The local file path for the new contents
        * @return this file description
        */
        FileDescription &set_content(const std::string &filename);

       /**
        * Change the file contents using a memory buffer
        *
        * This will alter the contents associated with this file description and replace them with a reference to the memory buffer.
        * It is up to the application to ensure that the data is retained in memory until it has finished with this file
        * description and the Transmitter has finished sending the file.
        * This will reset the TOI if the contents have changed.
        *
        * @param data The in memory buffer to use for the new file contents
        * @param data_length The length of the contents in the memory buffer
        * @return this file description
        */
        FileDescription &set_content(const char *data, size_t data_length);

       /**@{*/
       /**
        * Change the file contents using a vector
        *
        * This will alter the contents associated with this file description and replace them with a reference to the contents of
        * the vector. It is up to the application to ensure that the data is retained in memory until it has finished with this
        * file description and the Transmitter has finished sending the file.
        * This will reset the TOI if the contents have changed.
        *
        * @param data The vector to use for the new file contents
        * @return this file description
        */
        FileDescription &set_content(const std::vector<char> &data);
        FileDescription &set_content(const std::vector<unsigned char> &data);
       /**@}*/

       /**
        * Change the file content type
        *
        * This will set the `Content-Type` that is associated with this file.
        *
        * @param content_type The content type to set.
        * @return this file description
        */
        FileDescription &set_content_type(const std::string &content_type);

       /**
        * Change the file expiry time
        *
        * @param expiry_time The expiry time of the file in the FLUTE session.
        * @return this file description
        */
        FileDescription &set_expiry_time(const date_time_type &expiry_time);

       /**
        *  Get the currently set expiry time
        *
        *  @return the expiry time of this file.
        */
        date_time_type get_expiry_time() const;

       /**
        *  Set the ETag value for the file
        *
        *  Set to the empty string to remove the ETag.
        *
        *  @param etag The ETag to set
        *  @return this file description
        */
        FileDescription &set_etag(const std::string &etag);

       /**
        *  Get the current ETag value
        *
        *  @return The current ETag value for the file
        */
        const std::string &get_etag() const;

      protected:
        friend class Transmitter;
       /**
        * Set the TSI (used by the Transmitter class)
        *
        * @param val The new TSI value
        * @return this file description
        */
        FileDescription &tsi(uint64_t val) { _tsi = val; return *this; };
       /**
        * Set the TOI (used by the Transmitter class)
        *
        * @param val The new TOI value
        * @return this file description
        */
        FileDescription &toi(uint32_t val) { _file_entry.toi = val; return *this; };

       /**
        * Merge the FecOti values
        *
        * Takes any values that are unset in _file_entry.fec_oti from @p fec_oti.
        *
        * @param fec_oti The FecOti to merge values from.
        */
        FileDescription &merge_fec_oti(const FecOti &fec_oti);

      private:
        void _attach_file(const std::string &filename);
        void _free_file_data();
        void _calculate_file_entry();

        std::optional<uint64_t> _tsi; //< The last TSI this file was associated with
        FileDeliveryTable::FileEntry _file_entry; //< The FDT File entry values to use
        CompressionAlgorithm _compression_type;   //< The compression to apply to _data
        std::string _filename;                    //< The filename that _data was loaded from, empty for no local file
        int _file_handle;                         //< The file handle of the open _filename
        const char *_data;                        //< The uncompressed file contents (may be mapped file)
        size_t _data_length;                      //< The length of the uncompressed file contents
      };

     /**
      *  Definition of a file transmission completion callback function that can be
      *  registered through ::register_completion_callback.
      *
      *  @param toi TOI of the file that has completed transmission
      */
      typedef std::function<void(uint32_t)> completion_callback_t;

     /**
      *  Default constructor.
      *
      *  @param address Target multicast address
      *  @param port Target port
      *  @param tsi TSI value for the session
      *  @param mtu Path MTU to size FLUTE packets for
      *  @param rate_limit Transmit rate limit (in kbps)
      *  @param io_context Boost io_context to run the socket operations in (must be provided by the caller)
      *  @param tunnel_endpoint Tunnelling endpoint address (default: no tunnelling)
      *  @param fdt_namespace Which XML namespace to use for the FDT (default: none)
      */
      Transmitter( const std::string& address,
          short port, uint64_t tsi, unsigned short mtu,
          uint32_t rate_limit,
          boost::asio::io_context& io_context,
          const std::optional<boost::asio::ip::udp::endpoint> &tunnel_endpoint = std::nullopt,
          FdtNamespace fdt_namespace = FileDeliveryTable::FDT_NS_NONE);

     /**
      *  Default destructor.
      */
      virtual ~Transmitter();

     /**
      *  Enable IPSEC ESP encryption of FLUTE payloads.
      *
      *  @param spi Security Parameter Index value to use
      *  @param key AES key as a hex string (without leading 0x). Must be an even number of characters long.
      */
      void enable_ipsec( uint32_t spi, const std::string& aes_key);

     /**
      *  Transmit a file (deprecated).
      *
      *  @deprecated send(FileDescription*) should be used instead.
      *
      *  The caller must ensure the data buffer passed here remains valid until the completion callback
      *  for this file is called.
      *
      *  @param content_location URI to set in the content location field of the generated FDT entry
      *  @param content_type MIME type to set in the content type field of the generated FDT entry
      *  @param expires Expiry timestamp (based on NTP epoch)
      *  @param data Pointer to the data buffer (managed by caller)
      *  @param length Length of the data buffer (in bytes)
      *
      *  @return TOI of the file
      */
      uint16_t send(const std::string& content_location,
          const std::string& content_type,
          uint32_t expires,
          char* data,
          size_t length);

     /**
      *  Transmit a file.
      *  The caller must ensure the file description passed here remains valid until the completion callback
      *  for this file description is called.
      *
      *  The file description object passed in @a file_description may be updated by the Transmitter until the
      *  completion callback for this file description is called.
      *
      *  If a file description is reused then the TOI of the previous use is reused. This allows resends or
      *  updates to existing files to be transmitted.
      *
      *  @param file_description The file description object for the file to send
      *  @return TOI of the file.
      */
      uint16_t send(const std::shared_ptr<FileDescription> &file_description);

     /**
      *  Convenience function to get the current timestamp for expiry calculation
      *
      *  @return seconds since the NTP epoch
      */
      uint64_t seconds_since_epoch();

     /**
      *  Register a callback for file transmission completion notifications
      *
      *  @param cb Function to call on file completion
      */
      void register_completion_callback(completion_callback_t cb) { _completion_cb = cb; };

    private:
      void send_fdt();
      void send_next_packet();
      void fdt_send_tick();

      void file_transmitted(uint32_t toi);

      void handle_send_to(const boost::system::error_code& error);
      boost::asio::ip::udp::endpoint _endpoint;
      boost::asio::ip::udp::socket _socket;
      boost::asio::io_context& _io_context;
      boost::asio::deadline_timer _send_timer;
      boost::asio::deadline_timer _fdt_timer;

      uint64_t _tsi;
      uint16_t _mtu;

      std::unique_ptr<FileDeliveryTable> _fdt;
      std::map<uint32_t, std::shared_ptr<File>> _files;
      std::mutex _files_mutex;

      unsigned _fdt_repeat_interval = 5;
      uint16_t _toi = 1;

      uint32_t _max_payload;
      FecOti _fec_oti;

      completion_callback_t _completion_cb = nullptr;
      std::string _mcast_address;

      uint32_t _rate_limit = 0;
      std::optional<boost::asio::ip::udp::endpoint> _tunnel_endpoint = std::nullopt;
      boost::asio::ip::address _tunnel_local_address;
  };

} // end namespace LibFlute
