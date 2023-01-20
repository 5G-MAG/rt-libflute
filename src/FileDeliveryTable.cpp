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
#include "FileDeliveryTable.h"
#include "tinyxml2.h"
#include <iostream>
#include <string>
#include "spdlog/spdlog.h"

LibFlute::FileDeliveryTable::FileDeliveryTable(uint32_t instance_id, FecOti fec_oti)
  : _instance_id( instance_id )
  , _global_fec_oti( fec_oti )
{
  switch (fec_oti.encoding_id){
#ifdef RAPTOR_ENABLED
    case FecScheme::Raptor:
    _fdt_fec_transformer = new RaptorFEC();
    break;
#endif
    default:
    _fdt_fec_transformer = 0;
    break;
  }
}

LibFlute::FileDeliveryTable::~FileDeliveryTable() {
  if (_fdt_fec_transformer) {
    delete _fdt_fec_transformer;
    _fdt_fec_transformer = 0;
  }
}

LibFlute::FileDeliveryTable::FileDeliveryTable(uint32_t instance_id, char* buffer, size_t len) 
  : _instance_id( instance_id )
{
  tinyxml2::XMLDocument doc(true, tinyxml2::COLLAPSE_WHITESPACE);
  doc.Parse(buffer, len);
  auto fdt_instance = doc.FirstChildElement("FDT-Instance");
  _expires = std::stoull(fdt_instance->Attribute("Expires"));

  spdlog::debug("Received new FDT with instance ID {}: {}", instance_id, buffer);

  uint8_t def_fec_encoding_id = 0;
  auto val = fdt_instance->Attribute("FEC-OTI-FEC-Encoding-ID");
  if (val != nullptr) {
    def_fec_encoding_id = strtoul(val, nullptr, 0);
  }

  uint32_t def_fec_max_source_block_length = 0;
  val = fdt_instance->Attribute("FEC-OTI-Maximum-Source-Block-Length");
  if (val != nullptr) {
    def_fec_max_source_block_length = strtoul(val, nullptr, 0);
  }

  uint32_t def_fec_encoding_symbol_length = 0;
  val = fdt_instance->Attribute("FEC-OTI-Encoding-Symbol-Length");
  if (val != nullptr) {
    def_fec_encoding_symbol_length = strtoul(val, nullptr, 0);
  }

  for (auto file = fdt_instance->FirstChildElement("File"); 
      file != nullptr; file = file->NextSiblingElement("File")) {

    // required attributes
    auto toi_str = file->Attribute("TOI");
    if (toi_str == nullptr) {
      throw "Missing TOI attribute on File element";
    }
    uint32_t toi = strtoull(toi_str, nullptr, 0);

    auto content_location = file->Attribute("Content-Location");
    if (content_location == nullptr) {
      throw "Missing Content-Location attribute on File element";
    }

    uint32_t content_length = 0;
    val = file->Attribute("Content-Length");
    if (val != nullptr) {
      content_length = strtoull(val, nullptr, 0);
    }

    uint32_t transfer_length = 0;
    val = file->Attribute("Transfer-Length");
    if (val != nullptr) {
      transfer_length = strtoull(val, nullptr, 0);
    } else {
      transfer_length = content_length;
    }

    auto content_md5 = file->Attribute("Content-MD5");
    if (!content_md5) {
      content_md5 = "";
    }

    auto content_type = file->Attribute("Content-Type");
    if (!content_type) {
      content_type = "";
    }

    auto encoding_id = def_fec_encoding_id;
    val = file->Attribute("FEC-OTI-FEC-Encoding-ID");
    if (val != nullptr) {
      encoding_id = strtoul(val, nullptr, 0);
    }

    FecTransformer *fec_transformer = 0;

    switch (encoding_id){
#ifdef RAPTOR_ENABLED
      case (int) FecScheme::Raptor:
        fec_transformer = new RaptorFEC(); // corresponding delete calls in Receiver.cpp and destuctor function
      spdlog::debug("Received FDT entry for a raptor encoded file");
        break;
#endif
      default:
        break;
    }


    auto max_source_block_length = def_fec_max_source_block_length;
    val = file->Attribute("FEC-OTI-Maximum-Source-Block-Length");
    if (val != nullptr) {
      max_source_block_length = strtoul(val, nullptr, 0);
    }

    auto encoding_symbol_length = def_fec_encoding_symbol_length;
    val = file->Attribute("FEC-OTI-Encoding-Symbol-Length");
    if (val != nullptr) {
      encoding_symbol_length = strtoul(val, nullptr, 0);
    }

    if (fec_transformer && !fec_transformer->parse_fdt_info(file)) {
      throw "Failed to parse fdt info for specific FEC data";
    }
    uint32_t expires = 0;
    auto cc = file->FirstChildElement("mbms2007:Cache-Control");
    if (cc) {
      auto expires_elem = cc->FirstChildElement("mbms2007:Expires");
      if (expires_elem) {
        expires = strtoul(expires_elem->GetText(), nullptr, 0);
      }
    }

    FecOti fec_oti{
      (FecScheme)encoding_id,
        transfer_length,
        encoding_symbol_length,
        max_source_block_length
    };

    FileEntry fe{
      toi,
      std::string(content_location),
      content_length,
      std::string(content_md5),
      std::string(content_type),
      expires,
      fec_oti,
      fec_transformer
    };
    _file_entries.push_back(fe);
  }
}

auto LibFlute::FileDeliveryTable::add(FileEntry& fe) -> void
{
  _instance_id++;
  _file_entries.push_back(fe);
}

auto LibFlute::FileDeliveryTable::remove(uint32_t toi) -> void
{
  for (auto it = _file_entries.begin(); it != _file_entries.end();) {
    if (it->toi == toi) {
      if (it->fec_transformer) {
        delete it->fec_transformer;
        it->fec_transformer = 0;
      }
      it = _file_entries.erase(it);
    } else {
      ++it;
    }
  }
  _instance_id++;
}

auto LibFlute::FileDeliveryTable::to_string() const -> std::string {
  tinyxml2::XMLDocument doc;
  doc.InsertFirstChild( doc.NewDeclaration() );
  auto root = doc.NewElement("FDT-Instance");
  root->SetAttribute("Expires", std::to_string(_expires).c_str());
  root->SetAttribute("FEC-OTI-FEC-Encoding-ID", (unsigned)_global_fec_oti.encoding_id);
  root->SetAttribute("FEC-OTI-Maximum-Source-Block-Length", (unsigned)_global_fec_oti.max_source_block_length);
  root->SetAttribute("FEC-OTI-Encoding-Symbol-Length", (unsigned)_global_fec_oti.encoding_symbol_length);
  root->SetAttribute("xmlns:mbms2007", "urn:3GPP:metadata:2007:MBMS:FLUTE:FDT");
  doc.InsertEndChild(root);

  for (const auto& file : _file_entries) {
    auto f = doc.NewElement("File");
    f->SetAttribute("TOI", file.toi);
    f->SetAttribute("Content-Location", file.content_location.c_str());
    f->SetAttribute("Content-Length", file.content_length);
    f->SetAttribute("Transfer-Length", (unsigned)file.fec_oti.transfer_length);
    f->SetAttribute("Content-MD5", file.content_md5.c_str());
    f->SetAttribute("Content-Type", file.content_type.c_str());
    if(file.fec_transformer) {
      file.fec_transformer->add_fdt_info(f);
    }
    auto cc = doc.NewElement("mbms2007:Cache-Control");
    auto exp = doc.NewElement("mbms2007:Expires");
    exp->SetText(std::to_string(file.expires).c_str());
    cc->InsertEndChild(exp);
    f->InsertEndChild(cc);
    root->InsertEndChild(f);
  }


  tinyxml2::XMLPrinter printer;
  doc.Print(&printer);
  return std::string(printer.CStr());
}
