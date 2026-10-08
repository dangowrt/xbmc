/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WebPlayer.h"

#include "FileItem.h"
#include "ServiceBroker.h"
#include "WebPlayerDefs.h"
#include "WebVideoBuffer.h"
#include "application/ApplicationComponents.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "messaging/ApplicationMessenger.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include "platform/linux/waylandhost/WaylandBuffer.h"
#include "platform/linux/waylandhost/WaylandSurface.h"

using namespace KODI::WEB;
using namespace KODI::WAYLANDHOST;

namespace
{

constexpr int RENDER_BUFFERS = 3;

} // unnamed namespace

CWebPlayer::CWebPlayer(IPlayerCallback& callback)
  : IPlayer(callback),
    m_renderManager(m_clock, this)
{
}

CWebPlayer::~CWebPlayer()
{
  CloseFile();
}

bool CWebPlayer::OpenFile(const CFileItem& file, const CPlayerOptions& options)
{
  CloseFile();

  m_manager = CServiceBroker::GetAppComponents().GetComponent<CWebClientManager>();
  if (!m_manager || !m_manager->IsEnabled())
  {
    CLog::Log(LOGERROR, "CWebPlayer::{}: the web client is disabled", __FUNCTION__);
    return false;
  }

  m_item = std::make_unique<CFileItem>(file);
  m_options = options;
  m_stop = false;
  m_configured = false;
  m_surface = nullptr;
  m_url = file.GetDynPath();
  m_profile = file.GetProperty(PROPERTY_PROFILE).asString(DEFAULT_PROFILE);
  m_fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();

  m_processInfo.reset(CProcessInfo::CreateInstance());
  m_processInfo->SetDataCache(&CServiceBroker::GetDataCacheCore());
  m_processInfo->SetSpeed(1.0f);
  m_processInfo->SetTempo(1.0f);
  m_processInfo->SetFrameAdvance(false);
  m_processInfo->SetStateRealtime(true);
  m_pool = std::make_shared<CWebVideoBufferPool>();

  m_clock.SetSpeed(DVD_PLAYSPEED_NORMAL);
  m_clock.Discontinuity(0.0);
  m_renderManager.PreInit();

  if (!m_manager->RequestView(m_profile, *this))
  {
    m_renderManager.UnInit();
    return false;
  }

  CServiceBroker::GetWinSystem()->RegisterRenderLoop(this);
  m_playing = true;
  m_callback.OnPlayBackStarted(*m_item);
  return true;
}

bool CWebPlayer::CloseFile(bool reopen)
{
  if (!m_manager)
    return true;

  const bool wasPlaying = m_playing.exchange(false);
  m_stop = true;
  m_manager->ReleaseView(*this);
  if (m_surface)
  {
    m_manager->SetActive(m_profile, false);
    m_manager->LoadUrl(m_profile, "about:blank");
  }
  m_surface = nullptr;
  m_manager.reset();

  CServiceBroker::GetWinSystem()->UnregisterRenderLoop(this);
  m_renderManager.UnInit();
  m_pool.reset();
  CServiceBroker::GetDataCacheCore().Reset();
  m_processInfo.reset();

  if (wasPlaying)
    m_callback.OnPlayBackStopped();
  return true;
}

void CWebPlayer::Render(bool clear, uint32_t alpha, bool gui)
{
  m_renderManager.Render(clear, 0, alpha, gui);
}

void CWebPlayer::FlushRenderer()
{
  m_renderManager.Flush(true, true);
}

void CWebPlayer::SetRenderViewMode(int mode, float zoom, float par, float shift, bool stretch)
{
  if (!m_processInfo)
    return;

  m_processInfo->GetVideoSettingsLocked().SetViewMode(mode, zoom, par, shift, stretch);
  m_renderManager.SetVideoSettings(m_processInfo->GetVideoSettings());
  m_renderManager.SetViewMode(mode);
}

float CWebPlayer::GetRenderAspectRatio() const
{
  return m_renderManager.GetAspectRatio();
}

void CWebPlayer::TriggerUpdateResolution()
{
  std::string stereoMode;
  m_renderManager.TriggerUpdateResolution(0, 0, 0, stereoMode);
}

bool CWebPlayer::IsRenderingVideo() const
{
  return m_renderManager.IsConfigured();
}

void CWebPlayer::GetRects(CRect& source, CRect& dest, CRect& view) const
{
  m_renderManager.GetVideoRect(source, dest, view);
}

unsigned int CWebPlayer::GetOrientation() const
{
  return m_renderManager.GetOrientation();
}

bool CWebPlayer::Supports(ERENDERFEATURE feature) const
{
  return m_renderManager.Supports(feature);
}

bool CWebPlayer::Supports(ESCALINGMETHOD method) const
{
  return m_renderManager.Supports(method);
}

CVideoSettings CWebPlayer::GetVideoSettings() const
{
  return m_processInfo ? m_processInfo->GetVideoSettings() : CVideoSettings();
}

void CWebPlayer::SetVideoSettings(CVideoSettings& settings)
{
  if (!m_processInfo)
    return;

  m_processInfo->SetVideoSettings(settings);
  m_renderManager.SetVideoSettings(settings);
}

void CWebPlayer::FrameMove()
{
  m_renderManager.FrameMove();
}

void CWebPlayer::OnWebViewAvailable(const std::string& profile, CWaylandSurface& surface)
{
  CLog::Log(LOGINFO, "CWebPlayer::{}: view for profile \"{}\" loads {}", __FUNCTION__, profile,
            m_url);
  m_surface = &surface;
  m_manager->LoadUrl(profile, m_url);
  m_manager->SetActive(profile, true);
}

void CWebPlayer::OnWebViewUnavailable(const std::string& profile, const std::string& message)
{
  CLog::Log(LOGERROR, "CWebPlayer::{}: no view for profile \"{}\": {}", __FUNCTION__, profile,
            message);
  StopFromClient();
}

void CWebPlayer::OnWebViewGone(const std::string& profile)
{
  CLog::Log(LOGINFO, "CWebPlayer::{}: the view for profile \"{}\" is gone", __FUNCTION__, profile);
  m_surface = nullptr;
  StopFromClient();
}

void CWebPlayer::OnWebViewUnmapped(CWaylandSurface& surface)
{
  CLog::Log(LOGINFO, "CWebPlayer::{}: client surface \"{}\" is gone", __FUNCTION__,
            surface.GetTitle());
}

void CWebPlayer::OnWebViewUrl(const std::string& profile, const std::string& url)
{
  CLog::Log(LOGDEBUG, "CWebPlayer::{}: {}: {}", __FUNCTION__, profile, url);
}

void CWebPlayer::OnWebViewTitle(const std::string& profile, const std::string& title)
{
  CLog::Log(LOGDEBUG, "CWebPlayer::{}: {}: {}", __FUNCTION__, profile, title);
}

void CWebPlayer::OnWebViewProgress(const std::string& profile, uint32_t percent)
{
  CLog::Log(LOGDEBUG, "CWebPlayer::{}: {}: {}%", __FUNCTION__, profile, percent);
}

void CWebPlayer::OnWebViewNavigationState(const std::string& profile,
                                          bool canGoBack,
                                          bool canGoForward)
{
  CLog::Log(LOGDEBUG, "CWebPlayer::{}: {}: back {} forward {}", __FUNCTION__, profile, canGoBack,
            canGoForward);
}

void CWebPlayer::OnWebViewError(const std::string& profile,
                                uint32_t code,
                                const std::string& message)
{
  CLog::Log(LOGERROR, "CWebPlayer::{}: {}: error {}: {}", __FUNCTION__, profile, code, message);
}

void CWebPlayer::OnWebViewFrame(CWaylandSurface& surface, std::shared_ptr<CWaylandBuffer> buffer)
{
  if (&surface != m_surface || m_stop || !m_pool)
  {
    buffer->Unlock();
    return;
  }

  auto* videoBuffer = static_cast<CWebVideoBuffer*>(m_pool->Get());
  videoBuffer->SetBuffer(buffer);

  VideoPicture picture;
  picture.Reset();
  picture.videoBuffer = videoBuffer;
  picture.iWidth = buffer->GetWidth();
  picture.iHeight = buffer->GetHeight();
  picture.iDisplayWidth = picture.iWidth;
  picture.iDisplayHeight = picture.iHeight;
  picture.pixelFormat = AV_PIX_FMT_DRM_PRIME;
  picture.color_space = AVCOL_SPC_BT709;
  picture.color_range = 1;
  picture.color_primaries = AVCOL_PRI_BT709;
  picture.color_transfer = AVCOL_TRC_BT709;
  picture.chroma_position = AVCHROMA_LOC_UNSPECIFIED;
  picture.pts = m_clock.GetClock();
  picture.dts = DVD_NOPTS_VALUE;
  videoBuffer->SetPictureParams(picture);

  if (!m_configured)
  {
    CLog::Log(LOGINFO, "CWebPlayer::{}: first frame {}x{} format {:#x} modifier {:#x} planes {}",
              __FUNCTION__, buffer->GetWidth(), buffer->GetHeight(), buffer->GetFormat(),
              buffer->GetModifier(), buffer->GetPlaneCount());
    if (!ConfigureRenderer(picture))
      return;
  }

  if (!m_renderManager.AddVideoPicture(picture, m_stop, VS_INTERLACEMETHOD_NONE, false))
    CLog::Log(LOGDEBUG, "CWebPlayer::{}: frame dropped, no free render buffer", __FUNCTION__);
}

bool CWebPlayer::ConfigureRenderer(const VideoPicture& picture)
{
  m_processInfo->SetVideoDimensions(picture.iWidth, picture.iHeight);
  m_processInfo->SetVideoDAR(static_cast<float>(picture.iWidth) / picture.iHeight);
  m_processInfo->SetVideoDecoderName("wayland", true);
  m_processInfo->SetVideoPixelFormat("drm_prime");
  m_processInfo->SetVideoFps(m_fps);
  m_processInfo->SetVideoInterlaced(false);
  m_processInfo->SetVideoDeintMethod("none");

  if (!m_renderManager.Configure(picture, m_fps, 0, RENDER_BUFFERS))
  {
    CLog::Log(LOGERROR, "CWebPlayer::{}: no renderer for the client's {}x{} buffers", __FUNCTION__,
              picture.iWidth, picture.iHeight);
    m_stop = true;
    return false;
  }

  m_configured = true;
  m_renderManager.ShowVideo(true);
  if (m_options.fullscreen)
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_SWITCHTOFULLSCREEN);
  m_callback.OnAVStarted(*m_item);
  return true;
}

void CWebPlayer::StopFromClient()
{
  if (!m_playing.exchange(false))
    return;

  m_callback.OnPlayBackEnded();
}

void CWebPlayer::GetDebugInfo(std::string& audio, std::string& video, std::string& general)
{
  audio.clear();
  video = "web client frames through the Wayland host";
  general.clear();
}

void CWebPlayer::UpdateClockSync(bool enabled)
{
  m_processInfo->SetRenderClockSync(enabled);
}

void CWebPlayer::UpdateRenderInfo(CRenderInfo& info)
{
  m_processInfo->UpdateRenderInfo(info);
}

void CWebPlayer::UpdateRenderBuffers(int queued, int discard, int free)
{
  m_processInfo->UpdateRenderBuffers(queued, discard, free);
}

void CWebPlayer::UpdateGuiRender(bool gui)
{
  m_processInfo->SetGuiRender(gui);
}

void CWebPlayer::UpdateVideoRender(bool video)
{
  m_processInfo->SetVideoRender(video);
}
