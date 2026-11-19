// libflute - FLUTE/ALC library
//
// Copyright (C) 2026 5G-MAG Association (Jordi J. Gimenez <gimenez@5g-mag.com>)
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
// The attributes that TS 26.346 defines at both the FDT-Instance level and the File level are read at both levels,
// and a value given on the File element takes precedence over the one of the FDT-Instance.
//
// TS 26.346 V19.3.0 clause L.4.2: "The following FDT attributes, defined at both the FDT-Instance and File levels,
// shall be carried in the FDT sent by the FLUTE sender, under either the File-Instance or File element, and shall be
// supported by the FLUTE receiver:" (Content-Type is the first of them), and, for Content-Encoding set to 'gzip',
// "...may be carried in the FDT sent by the FLUTE sender, under either the File-Instance or File element, and shall be
// supported by the FLUTE receiver:".

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "FileDeliveryTable.h"

namespace
{
    using LibFlute::FileDeliveryTable;

    /// An FDT Instance of the profiled schema with one File element; the attributes are the ones given.
    std::string fdt_xml(const std::string& instance_attributes, const std::string& file_attributes)
    {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
               "<FDT-Instance xmlns=\"urn:3GPP:metadata:2022:FLUTE:FDT\" Expires=\"3900000000\" " +
               instance_attributes +
               ">"
               "<File TOI=\"1\" Content-Location=\"http://example.invalid/a\" Content-Length=\"100\" " +
               file_attributes +
               "/>"
               "<schemaVersion>3</schemaVersion></FDT-Instance>";
    }

    FileDeliveryTable::FileEntry only_entry(const std::string& xml)
    {
        std::vector<char> buf(xml.begin(), xml.end());
        FileDeliveryTable fdt(1, buf.data(), buf.size());
        EXPECT_EQ(fdt.file_entries().size(), 1U);
        return fdt.file_entries().at(0);
    }
} // namespace

TEST(FdtInstanceAttributeTest, ContentTypeOfTheFdtInstanceApplies)
{
    auto entry = only_entry(fdt_xml("Content-Type=\"video/mp4\"", ""));
    EXPECT_EQ(entry.content_type, "video/mp4");
}

TEST(FdtInstanceAttributeTest, ContentTypeOfTheFileTakesPrecedence)
{
    auto entry = only_entry(fdt_xml("Content-Type=\"video/mp4\"", "Content-Type=\"application/dash+xml\""));
    EXPECT_EQ(entry.content_type, "application/dash+xml");
}

TEST(FdtInstanceAttributeTest, NoContentTypeStaysEmpty)
{
    auto entry = only_entry(fdt_xml("", ""));
    EXPECT_TRUE(entry.content_type.empty());
}

TEST(FdtInstanceAttributeTest, ContentEncodingGzipOfTheFdtInstanceApplies)
{
    auto entry = only_entry(fdt_xml("Content-Encoding=\"gzip\"", "Transfer-Length=\"60\""));
    EXPECT_EQ(entry.content_encoding, "gzip");
}

TEST(FdtInstanceAttributeTest, ContentEncodingOfTheFileTakesPrecedence)
{
    auto entry = only_entry(fdt_xml("Content-Encoding=\"gzip\"", "Content-Encoding=\"deflate\" Transfer-Length=\"60\""));
    EXPECT_EQ(entry.content_encoding, "deflate");
}

// File-ETag, TS 26.346 V19.3.0 clause L.6.2: "The File-ETag represents the value of the entity tag as specified in section
// 8.8.3 of RFC 9110". The profiled schema of clause L.6.1 declares it without a namespace; the extended schema of clause
// 7.2.10 imports it as mbms2012:File-ETag.
TEST(FileEtagTest, UnqualifiedAttributeOfTheProfiledSchemaIsRead)
{
    auto entry = only_entry(fdt_xml("", "File-ETag=\"v1\""));
    EXPECT_EQ(entry.etag, "v1");
}

TEST(FileEtagTest, QualifiedAttributeOfTheExtendedSchemaIsStillRead)
{
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<FDT-Instance xmlns=\"urn:IETF:metadata:2005:FLUTE:FDT\" xmlns:mbms2012=\"urn:3GPP:metadata:2012:MBMS:FLUTE:FDT\" "
        "Expires=\"3900000000\">"
        "<File TOI=\"1\" Content-Location=\"http://example.invalid/a\" Content-Length=\"100\" "
        "mbms2012:File-ETag=\"abc\"/>"
        "<schemaVersion>5</schemaVersion></FDT-Instance>";
    auto entry = only_entry(xml);
    EXPECT_EQ(entry.etag, "abc");
}
