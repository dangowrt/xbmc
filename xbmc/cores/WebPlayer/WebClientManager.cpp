/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WebClientManager.h"

#include "FileItem.h"
#include "FileItemList.h"
#include "ServiceBroker.h"
#include "addons/Addon.h"
#include "addons/AddonManager.h"
#include "addons/addoninfo/AddonType.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include "platform/linux/waylandhost/WaylandBuffer.h"
#include "platform/linux/waylandhost/WaylandSurface.h"

#if defined(HAVE_GBM)
#include "windowing/gbm/WinSystemGbm.h"
#include "windowing/gbm/drm/DRMUtils.h"

#include "platform/linux/input/LibInputSettings.h"
#endif

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

using namespace KODI::WEB;
using namespace KODI::WAYLANDHOST;

namespace
{

constexpr const char* PROFILE_ROOT = "special://profile/webprofiles/";

std::string FirstRenderNode()
{
  std::string path;
  drmDevicePtr devices[16] = {};
  const int count = drmGetDevices2(0, devices, std::size(devices));
  for (int i = 0; i < count && path.empty(); ++i)
  {
    if (devices[i]->available_nodes & (1 << DRM_NODE_RENDER))
      path = devices[i]->nodes[DRM_NODE_RENDER];
  }
  if (count > 0)
    drmFreeDevices(devices, count);
  return path;
}

} // unnamed namespace

CWebClientManager::~CWebClientManager()
{
  Stop();
}

void CWebClientManager::Start()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  if (!settings->GetBool(CSettings::SETTING_WEB_ENABLED) || m_host)
    return;

  if (!StartHost())
    return;

  CServiceBroker::GetWinSystem()->RegisterRenderLoop(this);
  if (settings->GetBool(CSettings::SETTING_WEB_PRELOAD))
    Spawn();
}

void CWebClientManager::Stop()
{
  if (!m_host)
    return;

  CServiceBroker::GetWinSystem()->UnregisterRenderLoop(this);
  m_host->Stop();
  m_host.reset();

  std::unique_lock lock(m_lock);
  m_state = State::ABSENT;
  m_views.clear();
  m_surfaces.clear();
  m_observer = nullptr;
}

bool CWebClientManager::StartHost()
{
  WaylandHostConfig config;
  CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  config.width = gfx.GetWidth();
  config.height = gfx.GetHeight();
  config.refreshMilliHz = static_cast<int32_t>(std::lround(gfx.GetFPS() * 1000.0f));

#if defined(HAVE_GBM)
  if (auto* gbm = dynamic_cast<WINDOWING::GBM::CWinSystemGbm*>(CServiceBroker::GetWinSystem()))
  {
    if (auto drm = gbm->GetDrm())
      config.renderDevicePath = drm->GetRenderDevicePath();
    config.keyboardLayout = CServiceBroker::GetSettingsComponent()->GetSettings()->GetString(
        CLibInputSettings::SETTING_INPUT_LIBINPUTKEYBOARDLAYOUT);
  }
#endif
  if (config.renderDevicePath.empty())
    config.renderDevicePath = FirstRenderNode();
  if (config.renderDevicePath.empty())
  {
    CLog::Log(LOGERROR, "CWebClientManager::{}: no render device for the clients", __FUNCTION__);
    return false;
  }

  CLog::Log(LOGINFO, "CWebClientManager::{}: hosting {}x{} at {} mHz on {}", __FUNCTION__,
            config.width, config.height, config.refreshMilliHz, config.renderDevicePath);

  m_host = std::make_unique<CWaylandHost>(*this, std::move(config));
  if (m_host->Start())
    return true;

  m_host.reset();
  return false;
}

void CWebClientManager::Spawn()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const std::string command = settings->GetString(CSettings::SETTING_WEB_CLIENTCOMMAND);
  CLog::Log(LOGINFO, "CWebClientManager::{}: starting {}", __FUNCTION__, command);
  m_state = State::STARTING;
  // the client keeps profile data under XDG_DATA_HOME; Kodi's profile is
  // where it survives restarts and follows the Kodi profile in use
  std::map<std::string, std::string> env{
      {"WPE_PLATFORM", "wayland"},
      {"XDG_DATA_HOME", CSpecialProtocol::TranslatePath("special://profile/webdata")},
      {"XDG_CACHE_HOME", CSpecialProtocol::TranslatePath("special://temp/webcache")},
      {"KODI_WEBVIEW_SOFTWARE_CODECS",
       settings->GetBool(CSettings::SETTING_WEB_SOFTWARECODECS) ? "1" : "0"}};
  m_host->SpawnClient({command}, std::move(env));
}

CWebClientManager::State CWebClientManager::GetState() const
{
  std::unique_lock lock(m_lock);
  return m_state;
}

bool CWebClientManager::RequestView(const std::string& profile, IWebViewObserver& observer)
{
  if (!m_host)
    return false;

  std::unique_lock lock(m_lock);
  m_observer = &observer;
  m_observedProfile = profile;
  m_wantPending = false;

  const auto view = m_views.find(profile);
  if (view != m_views.end() && view->second)
  {
    CWaylandSurface& surface = *view->second;
    lock.unlock();
    observer.OnWebViewAvailable(profile, surface);
    return true;
  }

  switch (m_state)
  {
    case State::READY:
      m_host->WebWantView(profile, {});
      break;
    case State::STARTING:
      m_wantPending = true;
      break;
    case State::ABSENT:
    case State::FAILED:
      m_respawned = false;
      m_wantPending = true;
      Spawn();
      break;
  }
  return true;
}

void CWebClientManager::ReleaseView(IWebViewObserver& observer)
{
  std::unique_lock lock(m_lock);
  if (m_observer == &observer)
  {
    m_observer = nullptr;
    m_wantPending = false;
  }
}

void CWebClientManager::LoadUrl(const std::string& profile, const std::string& url)
{
  if (m_host)
    m_host->WebViewLoadUrl(profile, url);
}

void CWebClientManager::Navigate(const std::string& profile, WebNavigation action)
{
  if (m_host)
    m_host->WebViewNavigate(profile, action);
}

void CWebClientManager::SetActive(const std::string& profile, bool active)
{
  if (m_host)
    m_host->WebViewSetActive(profile, active);
}

void CWebClientManager::FrameMove()
{
  if (m_host)
    m_host->FrameDone();
}

IWebViewObserver* CWebClientManager::ObserverOf(const std::string& profile)
{
  return m_observer && m_observedProfile == profile ? m_observer : nullptr;
}

void CWebClientManager::OnSurfaceMapped(CWaylandSurface& surface)
{
  CLog::Log(LOGINFO, "CWebClientManager::{}: client surface \"{}\" is up", __FUNCTION__,
            surface.GetTitle());
}

void CWebClientManager::OnSurfaceUnmapped(CWaylandSurface& surface)
{
  std::unique_lock lock(m_lock);
  const auto entry = m_surfaces.find(&surface);
  IWebViewObserver* observer = entry == m_surfaces.end() ? nullptr : ObserverOf(entry->second);
  lock.unlock();
  if (observer)
    observer->OnWebViewUnmapped(surface);
}

void CWebClientManager::OnSurfaceFrame(CWaylandSurface& surface,
                                       std::shared_ptr<CWaylandBuffer> buffer)
{
  std::unique_lock lock(m_lock);
  const auto entry = m_surfaces.find(&surface);
  IWebViewObserver* observer = entry == m_surfaces.end() ? nullptr : ObserverOf(entry->second);
  lock.unlock();
  if (observer)
    observer->OnWebViewFrame(surface, std::move(buffer));
  else
    buffer->Unlock();
}

void CWebClientManager::OnClientExited(pid_t pid, int status)
{
  std::unique_lock lock(m_lock);
  CLog::Log(LOGWARNING, "CWebClientManager::{}: the web client (pid {}) exited with status {}",
            __FUNCTION__, pid, status);
  m_views.clear();
  m_surfaces.clear();
  IWebViewObserver* observer = m_observer;
  const std::string profile = m_observedProfile;

  // one restart per request; a client that fails again stays down until
  // the next web request
  if (m_state == State::READY && !m_respawned)
  {
    m_respawned = true;
    m_wantPending = observer != nullptr;
    Spawn();
  }
  else
  {
    m_state = State::FAILED;
    m_wantPending = false;
  }
  lock.unlock();

  if (observer)
    observer->OnWebViewGone(profile);
}

void CWebClientManager::OnWebClientReady()
{
  std::unique_lock lock(m_lock);
  m_state = State::READY;
  // a view the client created on its own before ready answers the request
  const bool want = m_wantPending && m_views.find(m_observedProfile) == m_views.end();
  m_wantPending = false;
  const std::string profile = m_observedProfile;
  lock.unlock();
  if (want)
    m_host->WebWantView(profile, {});
}

bool CWebClientManager::ConfigureViewFromAddon(const std::string& profile)
{
  ADDON::AddonPtr addon;
  if (!CServiceBroker::GetAddonMgr().GetAddon(profile, addon, ADDON::AddonType::WEBAPP,
                                              ADDON::OnlyEnabled::CHOICE_YES))
    return false;

  const auto webapp = std::dynamic_pointer_cast<ADDON::CAddon>(addon);
  const ADDON::CAddonType* type = webapp ? webapp->Type(ADDON::AddonType::WEBAPP) : nullptr;
  if (!type)
    return false;

  const std::string identity = webapp->GetSetting("identity");
  std::string userAgent = identity == "custom" ? webapp->GetSetting("useragent") : "";
  for (const auto& [element, values] : type->GetValues())
  {
    if (element == "useragent" && userAgent.empty() &&
        values.GetValue("useragent@id").asString() == identity)
      userAgent = values.GetValue("useragent").asString();
  }
  if (!userAgent.empty())
    m_host->WebViewSetUserAgent(profile, userAgent);

  for (const auto& [element, values] : type->GetValues())
  {
    if (element == "userscript")
      SendProfileFile(
          profile,
          URIUtils::AddFileToFolder(webapp->Path(), values.GetValue("userscript").asString()),
          true);
    else if (element == "contentfilter")
      SendProfileFile(
          profile,
          URIUtils::AddFileToFolder(webapp->Path(), values.GetValue("contentfilter").asString()),
          false);
  }
  return true;
}

void CWebClientManager::ConfigureView(const std::string& profile)
{
  if (ConfigureViewFromAddon(profile))
    return;

  const std::string root = PROFILE_ROOT + profile + "/";
  if (!XFILE::CDirectory::Exists(root))
    return;

  std::vector<uint8_t> userAgent;
  if (XFILE::CFile().LoadFile(root + "user-agent", userAgent) > 0)
  {
    std::string value(userAgent.begin(), userAgent.end());
    m_host->WebViewSetUserAgent(profile, StringUtils::Trim(value));
  }
  SendProfileFiles(profile, root + "scripts/", ".js", true);
  SendProfileFiles(profile, root + "filters/", ".json", false);
}

void CWebClientManager::SendProfileFiles(const std::string& profile,
                                         const std::string& path,
                                         const std::string& extension,
                                         bool scripts)
{
  CFileItemList items;
  if (!XFILE::CDirectory::GetDirectory(path, items, extension, XFILE::DIR_FLAG_NO_FILE_DIRS))
    return;

  std::vector<std::string> files;
  for (const auto& item : items)
  {
    if (!item->IsFolder())
      files.push_back(item->GetPath());
  }
  std::sort(files.begin(), files.end());

  for (const std::string& file : files)
    SendProfileFile(profile, file, scripts);
}

void CWebClientManager::SendProfileFile(const std::string& profile,
                                        const std::string& file,
                                        bool script)
{
  const int fd = open(CSpecialProtocol::TranslatePath(file).c_str(), O_RDONLY | O_CLOEXEC);
  struct stat st = {};
  if (fd < 0 || fstat(fd, &st) != 0)
  {
    CLog::Log(LOGERROR, "CWebClientManager::{}: cannot read {}", __FUNCTION__, file);
    if (fd >= 0)
      close(fd);
    return;
  }

  const std::string name = URIUtils::GetFileName(file);
  const auto size = static_cast<uint32_t>(st.st_size);
  if (script)
    m_host->WebViewAddUserScript(profile, name, fd, size);
  else
    m_host->WebViewAddContentFilter(profile, name, fd, size);
}

void CWebClientManager::OnWebViewCreated(const std::string& profile, CWaylandSurface& surface)
{
  ConfigureView(profile);

  std::unique_lock lock(m_lock);
  m_views[profile] = &surface;
  m_surfaces[&surface] = profile;
  IWebViewObserver* observer = ObserverOf(profile);
  if (observer)
    m_wantPending = false;
  lock.unlock();
  if (observer)
    observer->OnWebViewAvailable(profile, surface);
}

void CWebClientManager::OnWebViewUnavailable(const std::string& profile, const std::string& message)
{
  std::unique_lock lock(m_lock);
  IWebViewObserver* observer = ObserverOf(profile);
  lock.unlock();
  if (observer)
    observer->OnWebViewUnavailable(profile, message);
}

void CWebClientManager::OnWebViewUrl(const std::string& profile, const std::string& url)
{
  std::unique_lock lock(m_lock);
  IWebViewObserver* observer = ObserverOf(profile);
  lock.unlock();
  if (observer)
    observer->OnWebViewUrl(profile, url);
}

void CWebClientManager::OnWebViewTitle(const std::string& profile, const std::string& title)
{
  std::unique_lock lock(m_lock);
  IWebViewObserver* observer = ObserverOf(profile);
  lock.unlock();
  if (observer)
    observer->OnWebViewTitle(profile, title);
}

void CWebClientManager::OnWebViewProgress(const std::string& profile, uint32_t percent)
{
  std::unique_lock lock(m_lock);
  IWebViewObserver* observer = ObserverOf(profile);
  lock.unlock();
  if (observer)
    observer->OnWebViewProgress(profile, percent);
}

void CWebClientManager::OnWebViewNavigationState(const std::string& profile,
                                                 bool canGoBack,
                                                 bool canGoForward)
{
  std::unique_lock lock(m_lock);
  IWebViewObserver* observer = ObserverOf(profile);
  lock.unlock();
  if (observer)
    observer->OnWebViewNavigationState(profile, canGoBack, canGoForward);
}

void CWebClientManager::OnWebViewError(const std::string& profile,
                                       uint32_t code,
                                       const std::string& message)
{
  std::unique_lock lock(m_lock);
  IWebViewObserver* observer = ObserverOf(profile);
  lock.unlock();
  if (observer)
    observer->OnWebViewError(profile, code, message);
}

void CWebClientManager::OnWebViewDestroyed(const std::string& profile)
{
  std::unique_lock lock(m_lock);
  const auto view = m_views.find(profile);
  if (view != m_views.end())
  {
    m_surfaces.erase(view->second);
    m_views.erase(view);
  }
  IWebViewObserver* observer = ObserverOf(profile);
  lock.unlock();
  if (observer)
    observer->OnWebViewGone(profile);
}
