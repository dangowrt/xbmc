/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "UplinkAddress.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <cstdlib>
#include <cstring>
#include <string>

#if defined(TARGET_POSIX)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#if defined(HAS_UPLINKDNS)
#include <uplinkdns.h>
#endif

namespace KODI::NETWORK
{

namespace
{
#if defined(TARGET_POSIX)
bool PrefixMatch(const void* address, const void* network, unsigned int bits)
{
  const auto* a = static_cast<const unsigned char*>(address);
  const auto* n = static_cast<const unsigned char*>(network);
  const unsigned int bytes = bits / 8;

  if (std::memcmp(a, n, bytes) != 0)
    return false;

  if (bits % 8 == 0)
    return true;

  const unsigned char mask = static_cast<unsigned char>(0xff << (8 - bits % 8));
  return (a[bytes] & mask) == (n[bytes] & mask);
}

const void* InterfaceAddress(const sockaddr* sa)
{
  if (sa->sa_family == AF_INET)
    return &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr;

  return &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr;
}

std::string FirstAddress(int family,
                         const std::string& ifname,
                         const void* network,
                         unsigned int bits)
{
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0)
    return {};

  std::string address;
  for (ifaddrs* interface = interfaces; interface && address.empty();
       interface = interface->ifa_next)
  {
    if (!interface->ifa_addr || interface->ifa_addr->sa_family != family ||
        !(interface->ifa_flags & IFF_UP))
      continue;

    const void* candidate = InterfaceAddress(interface->ifa_addr);
    if (!ifname.empty() && ifname != interface->ifa_name)
      continue;

    if (network && !PrefixMatch(candidate, network, bits))
      continue;

    char text[INET6_ADDRSTRLEN] = {};
    if (inet_ntop(family, candidate, text, sizeof(text)))
      address = text;
  }

  freeifaddrs(interfaces);
  return address;
}

std::string PrefixAddress(const std::string& key, int family)
{
  const size_t slash = key.find('/');
  const std::string base = key.substr(0, slash);
  const int bits = std::atoi(key.substr(slash + 1).c_str());
  in6_addr network = {};
  int prefixFamily = AF_INET;

  if (inet_pton(AF_INET, base.c_str(), &network) != 1)
  {
    prefixFamily = AF_INET6;
    if (inet_pton(AF_INET6, base.c_str(), &network) != 1)
    {
      CLog::LogF(LOGERROR, "malformed uplink specification '{}'", key);
      return {};
    }
  }

  const int maxBits = prefixFamily == AF_INET ? 32 : 128;
  if (bits < 0 || bits > maxBits)
  {
    CLog::LogF(LOGERROR, "malformed uplink specification '{}'", key);
    return {};
  }

  if (family != AF_UNSPEC && family != prefixFamily)
    return {};

  return FirstAddress(prefixFamily, {}, &network, static_cast<unsigned int>(bits));
}

bool SyntheticName(const std::string& host, std::string& name)
{
#if defined(HAS_UPLINKDNS)
  sockaddr_in6 sin6 = {};
  sockaddr_in sin = {};
  char text[NI_MAXHOST] = {};
  int ret = -1;

  if (inet_pton(AF_INET, host.c_str(), &sin.sin_addr) == 1)
  {
    sin.sin_family = AF_INET;
    ret = uplinkdns_name_get(reinterpret_cast<sockaddr*>(&sin), sizeof(sin), text, sizeof(text));
  }
  else if (inet_pton(AF_INET6, host.c_str(), &sin6.sin6_addr) == 1)
  {
    sin6.sin6_family = AF_INET6;
    ret = uplinkdns_name_get(reinterpret_cast<sockaddr*>(&sin6), sizeof(sin6), text, sizeof(text));
  }

  if (ret != 0)
    return false;

  name = text;
  return true;
#else
  return false;
#endif
}
#endif
} // unnamed namespace

std::string UplinkKey(const std::string& spec)
{
  return spec.substr(0, spec.find(';'));
}

void RegisterUplink(const std::string& spec)
{
#if defined(HAS_UPLINKDNS)
  if (!spec.empty() && uplinkdns_uplink_add(spec.c_str()) < 0)
    CLog::LogF(LOGERROR, "invalid uplink specification '{}'", spec);
#endif
}

UplinkBinding ResolveUplink(const std::string& spec, int family)
{
  UplinkBinding binding;
  const std::string key = UplinkKey(spec);

  if (key.empty())
    return binding;

  RegisterUplink(spec);

#if !defined(TARGET_POSIX)
  binding.address = key;
#else
  in6_addr literal = {};

  if (inet_pton(AF_INET, key.c_str(), &literal) == 1)
  {
    if (family != AF_INET6)
      binding.address = key;
  }
  else if (inet_pton(AF_INET6, key.c_str(), &literal) == 1)
  {
    if (family != AF_INET)
      binding.address = key;
  }
  else if (key.find('/') != std::string::npos)
  {
    binding.address = PrefixAddress(key, family);
  }
  else if (if_nametoindex(key.c_str()) != 0)
  {
    binding.device = key;
  }

  if (!binding.IsValid())
    CLog::LogF(LOGWARNING, "uplink '{}' is not available", key);
#endif

  return binding;
}

UplinkBinding RequireUplink(const std::string& spec)
{
  UplinkBinding binding = ResolveUplink(spec, AF_UNSPEC);

  // no device carries the key of an unavailable uplink, so binding to it fails the request
  if (!binding.IsValid())
    binding.device = UplinkKey(spec);

  return binding;
}

std::string ResolveUplinkAddress(const std::string& spec)
{
  const UplinkBinding binding = ResolveUplink(spec, AF_INET);

#if defined(TARGET_POSIX)
  if (!binding.device.empty())
    return FirstAddress(AF_INET, binding.device, nullptr, 0);
#endif

  return binding.address;
}

bool IsDirectDestination(const std::string& host)
{
  if (host.empty())
    return true;

#if defined(TARGET_POSIX)
  std::string name;
  if (SyntheticName(host, name))
    return name.find('.') == std::string::npos;

  in6_addr address6 = {};
  if (inet_pton(AF_INET6, host.c_str(), &address6) == 1)
    return IN6_IS_ADDR_LOOPBACK(&address6) || IN6_IS_ADDR_LINKLOCAL(&address6) ||
           (address6.s6_addr[0] & 0xfe) == 0xfc;

  in_addr address4 = {};
  if (inet_pton(AF_INET, host.c_str(), &address4) != 1)
    return host.find('.') == std::string::npos;
#endif

  return URIUtils::IsHostOnLAN(host, LanCheckMode::ANY_PRIVATE_SUBNET);
}

std::string UplinkOfURL(const CURL& url)
{
  if (url.HasProtocolOption("local_dev"))
    return url.GetProtocolOption("local_dev");

  return url.GetProtocolOption("local_addr");
}

bool IsUplinkURL(const std::string& path)
{
  const CURL url(path);
  return url.IsProtocol("http") || url.IsProtocol("https") || url.IsProtocol("dav") ||
         url.IsProtocol("davs") || url.IsProtocol("ftp") || url.IsProtocol("ftps") ||
         url.IsProtocol("udp") || url.IsProtocol("rtp");
}

std::string UplinkPath(const std::string& path, const std::string& spec)
{
  if (spec.empty() || !IsUplinkURL(path))
    return path;

  const UplinkBinding uplink = RequireUplink(spec);
  CURL url(path);
  if (uplink.address.empty())
    url.SetProtocolOption("local_dev", uplink.device);
  else
    url.SetProtocolOption("local_addr", uplink.address);

  return url.Get();
}

std::string AddonUplinkPath(const std::string& addonId, const std::string& path)
{
  const auto settings = CServiceBroker::GetSettingsComponent();
  if (addonId.empty() || !settings || !settings->GetAdvancedSettings())
    return path;

  return UplinkPath(path, settings->GetAdvancedSettings()->GetAddonUplink(addonId));
}

} /* namespace KODI::NETWORK */
