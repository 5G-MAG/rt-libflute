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
#include <stdexcept>
#include <string>
#include <cstring>
#include <iostream>
#include <vector>
#include "spdlog/spdlog.h"
#include <netlink/netlink.h>
#include <netlink/attr.h>
#include <netlink/msg.h>
#include <linux/rtnetlink.h>
#include <linux/in.h>
#include <linux/xfrm.h>
#include <linux/ipsec.h>
#include <arpa/inet.h>
#include "IpSec.h"
#include <boost/algorithm/hex.hpp>

// Suppress warnings about SHA256 (via the legacy one-shot API) being
// deprecated in later versions of OpenSSL, consistent with Transmitter.cpp's
// use of the equally-deprecated one-shot MD5() API.
#define OPENSSL_SUPPRESS_DEPRECATED 1
#include <openssl/sha.h>

namespace LibFlute::IpSec {
  void configure_policy(uint32_t spi, const std::string& dest_address, Direction direction)
  {
    struct nl_sock *sk;
    struct nl_msg *msg;

    struct xfrm_userpolicy_info	xpinfo = {};
    xpinfo.lft.soft_byte_limit = XFRM_INF;
    xpinfo.lft.hard_byte_limit = XFRM_INF;
    xpinfo.lft.soft_packet_limit = XFRM_INF;
    xpinfo.lft.hard_packet_limit = XFRM_INF;
    xpinfo.dir = (direction == Direction::In) ? XFRM_POLICY_IN : XFRM_POLICY_OUT;

    xpinfo.sel.family = AF_INET;
    xpinfo.sel.saddr.a4 = INADDR_ANY;
    xpinfo.sel.daddr.a4 = inet_addr(dest_address.c_str());
    xpinfo.sel.prefixlen_d = 32;

    struct xfrm_user_tmpl tmpl = {};
    tmpl.id.daddr.a4 = inet_addr(dest_address.c_str());
    tmpl.id.spi = htonl(spi);
    tmpl.id.proto = IPPROTO_ESP;
    tmpl.saddr.a4 = INADDR_ANY;
    tmpl.reqid = spi;
    tmpl.mode = XFRM_MODE_TRANSPORT;
    tmpl.aalgos = (~(__u32)0);
    tmpl.ealgos = (~(__u32)0);
    tmpl.calgos = (~(__u32)0);
    tmpl.family = AF_INET;

    msg = nlmsg_alloc_simple(XFRM_MSG_UPDPOLICY, 0);
    nlmsg_append(msg, &xpinfo, sizeof(xpinfo), NLMSG_ALIGNTO);
    nla_put(msg, XFRMA_TMPL, sizeof(tmpl), &tmpl);

    sk = nl_socket_alloc();
    nl_connect(sk, NETLINK_XFRM);
    nl_send_auto(sk, msg);
    nlmsg_free(msg);
    nl_socket_free(sk); // BUG FIX: was never freed, leaking a netlink socket/fd on every call
  }
  void configure_state(uint32_t spi, const std::string& dest_address, Direction direction, const std::string& key,
                        const std::string& auth_key)
  {
    struct nl_sock *sk;
    struct nl_msg *msg;

    struct xfrm_usersa_info	xsinfo = {};

    xsinfo.sel.family = AF_INET;
    xsinfo.sel.saddr.a4 = INADDR_ANY;
    xsinfo.sel.daddr.a4 = inet_addr(dest_address.c_str());
    xsinfo.sel.prefixlen_d = 32;
    
    xsinfo.id.daddr.a4 = inet_addr(dest_address.c_str());
    xsinfo.id.spi = htonl(spi);
    xsinfo.id.proto = IPPROTO_ESP;
    
    xsinfo.saddr.a4 = INADDR_ANY;

    xsinfo.lft.soft_byte_limit = XFRM_INF;
    xsinfo.lft.hard_byte_limit = XFRM_INF;
    xsinfo.lft.soft_packet_limit = XFRM_INF;
    xsinfo.lft.hard_packet_limit = XFRM_INF;

    xsinfo.reqid = spi;
    xsinfo.family = AF_INET;
    xsinfo.mode = XFRM_MODE_TRANSPORT;

    std::vector<char> algo_buf(sizeof(struct xfrm_algo) + 512, 0);
    auto* algo = reinterpret_cast<struct xfrm_algo*>(algo_buf.data());

    std::vector<char> binary_key;
    for (unsigned int i = 0; i < key.length(); i += 2) {
      binary_key.emplace_back((char)strtol(key.substr(i, 2).c_str(), nullptr, 16));
    }
    if (binary_key.size() > 512) {
      throw std::runtime_error("Key is too long");
    }
    strcpy(algo->alg_name, "aes");
    algo->alg_key_len = binary_key.size() * 8;
    memcpy(algo->alg_key, &binary_key[0], binary_key.size());

    // RFC 6726 SS7.5: authentication SHALL also be present, not just encryption.
    // Configure HMAC-SHA256 (XFRMA_ALG_AUTH) alongside the AES encryption above.
    std::vector<char> auth_algo_buf(sizeof(struct xfrm_algo) + 512, 0);
    auto* auth_algo = reinterpret_cast<struct xfrm_algo*>(auth_algo_buf.data());

    std::vector<unsigned char> auth_key_bytes;
    if (!auth_key.empty()) {
      for (unsigned int i = 0; i < auth_key.length(); i += 2) {
        auth_key_bytes.push_back((unsigned char)strtol(auth_key.substr(i, 2).c_str(), nullptr, 16));
      }
    } else {
      // No separate auth key supplied: derive one deterministically from the
      // crypt key (distinct bytes from it, not the same secret reused as-is)
      // so authentication is still configured for existing single-key callers.
      static const std::string context = "libflute-ipsec-auth-key-v1";
      std::vector<unsigned char> input(binary_key.begin(), binary_key.end());
      input.insert(input.end(), context.begin(), context.end());
      unsigned char digest[SHA256_DIGEST_LENGTH];
      SHA256(input.data(), input.size(), digest);
      auth_key_bytes.assign(digest, digest + SHA256_DIGEST_LENGTH);
    }
    if (auth_key_bytes.size() > 512) {
      throw std::runtime_error("Authentication key is too long");
    }
    strcpy(auth_algo->alg_name, "hmac(sha256)");
    auth_algo->alg_key_len = auth_key_bytes.size() * 8;
    memcpy(auth_algo->alg_key, auth_key_bytes.data(), auth_key_bytes.size());

    msg = nlmsg_alloc_simple(XFRM_MSG_NEWSA, 0);
    nlmsg_append(msg, &xsinfo, sizeof(xsinfo), NLMSG_ALIGNTO);
    nla_put(msg, XFRMA_ALG_CRYPT, algo_buf.size(), algo);
    nla_put(msg, XFRMA_ALG_AUTH, auth_algo_buf.size(), auth_algo);

    sk = nl_socket_alloc();
    nl_connect(sk, NETLINK_XFRM);
    nl_send_auto(sk, msg);
    nlmsg_free(msg);
    nl_socket_free(sk); // BUG FIX: was never freed, leaking a netlink socket/fd on every call
  }

  void enable_esp(uint32_t spi, const std::string& dest_address, Direction direction, const std::string& key,
                   const std::string& auth_key)
  {
    configure_state(spi, dest_address, direction, key, auth_key);
    configure_policy(spi, dest_address, direction);
  }
};
