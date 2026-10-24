/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string>

class CURL;

namespace KODI::NETWORK
{

struct UplinkBinding
{
  std::string device;
  std::string address;

  bool IsValid() const { return !device.empty() || !address.empty(); }
};

std::string UplinkKey(const std::string& spec);
void RegisterUplink(const std::string& spec);
UplinkBinding ResolveUplink(const std::string& spec, int family);
UplinkBinding RequireUplink(const std::string& spec);
std::string ResolveUplinkAddress(const std::string& spec);
bool IsDirectDestination(const std::string& host);
std::string UplinkOfURL(const CURL& url);

} /* namespace KODI::NETWORK */
