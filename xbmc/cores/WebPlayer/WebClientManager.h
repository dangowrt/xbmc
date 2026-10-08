/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "application/IApplicationComponent.h"
#include "guilib/DispResource.h"
#include "jobs/Job.h"
#include "threads/CriticalSection.h"

#include "platform/linux/waylandhost/WaylandHost.h"

#include <map>
#include <memory>
#include <string>

namespace KODI
{
namespace WEB
{

/*!
 * \brief What a player wants to know about the view it asked for. All
 * methods run on the host thread.
 */
class IWebViewObserver
{
public:
  virtual ~IWebViewObserver() = default;

  virtual void OnWebViewAvailable(const std::string& profile,
                                  WAYLANDHOST::CWaylandSurface& surface) = 0;
  virtual void OnWebViewUnavailable(const std::string& profile, const std::string& message) = 0;
  virtual void OnWebViewGone(const std::string& profile) = 0;
  virtual void OnWebViewFrame(WAYLANDHOST::CWaylandSurface& surface,
                              std::shared_ptr<WAYLANDHOST::CWaylandBuffer> buffer) = 0;
  virtual void OnWebViewUnmapped(WAYLANDHOST::CWaylandSurface& surface) = 0;
  virtual void OnWebViewUrl(const std::string& profile, const std::string& url) = 0;
  virtual void OnWebViewTitle(const std::string& profile, const std::string& title) = 0;
  virtual void OnWebViewProgress(const std::string& profile, uint32_t percent) = 0;
  virtual void OnWebViewNavigationState(const std::string& profile,
                                        bool canGoBack,
                                        bool canGoForward) = 0;
  virtual void OnWebViewError(const std::string& profile,
                              uint32_t code,
                              const std::string& message) = 0;
};

/*!
 * \brief Runs the Wayland host for Kodi's lifetime and keeps the web client
 * warm: the client is started once the GUI is up, its views survive the
 * player that showed them, and a crashed client is started again once.
 */
class CWebClientManager : public IApplicationComponent,
                          public WAYLANDHOST::IWaylandHostObserver,
                          public IRenderLoop
{
public:
  enum class State
  {
    ABSENT,
    STARTING,
    READY,
    FAILED,
  };

  CWebClientManager() = default;
  ~CWebClientManager() override;

  void Start();
  void Stop();

  bool IsEnabled() const { return m_host != nullptr; }
  State GetState() const;
  WAYLANDHOST::CWaylandHost* GetHost() const { return m_host.get(); }

  /*!
   * \brief Ask for the view of a profile on behalf of an observer; the
   * answer comes through the observer, at once if the view exists
   */
  bool RequestView(const std::string& profile, IWebViewObserver& observer);
  void ReleaseView(IWebViewObserver& observer);

  void LoadUrl(const std::string& profile, const std::string& url);
  void Navigate(const std::string& profile, WAYLANDHOST::WebNavigation action);
  void SetActive(const std::string& profile, bool active);

  // implementation of IRenderLoop
  void FrameMove() override;

  // implementation of IWaylandHostObserver
  void OnSurfaceMapped(WAYLANDHOST::CWaylandSurface& surface) override;
  void OnSurfaceUnmapped(WAYLANDHOST::CWaylandSurface& surface) override;
  void OnSurfaceFrame(WAYLANDHOST::CWaylandSurface& surface,
                      std::shared_ptr<WAYLANDHOST::CWaylandBuffer> buffer) override;
  void OnClientExited(pid_t pid, int status) override;
  void OnWebClientReady() override;
  void OnWebViewCreated(const std::string& profile, WAYLANDHOST::CWaylandSurface& surface) override;
  void OnWebViewUnavailable(const std::string& profile, const std::string& message) override;
  void OnWebViewUrl(const std::string& profile, const std::string& url) override;
  void OnWebViewTitle(const std::string& profile, const std::string& title) override;
  void OnWebViewProgress(const std::string& profile, uint32_t percent) override;
  void OnWebViewNavigationState(const std::string& profile,
                                bool canGoBack,
                                bool canGoForward) override;
  void OnWebViewError(const std::string& profile,
                      uint32_t code,
                      const std::string& message) override;
  void OnWebViewDestroyed(const std::string& profile) override;
  void OnTextInputEnabled(const WAYLANDHOST::TextInputState& state) override;
  void OnTextInputUpdated(const WAYLANDHOST::TextInputState& state) override;
  void OnTextInputDisabled() override;

  // the keyboard dialog's life, off the host thread
  void OnKeyboardOpening(int coveredTop);
  void OnKeyboardText(const std::string& text);
  void OnKeyboardDone();

private:
  bool StartHost();
  void Spawn();
  void ConfigureView(const std::string& profile);
  bool ConfigureViewFromAddon(const std::string& profile);
  void SendProfileFiles(const std::string& profile,
                        const std::string& path,
                        const std::string& extension,
                        bool scripts);
  void SendProfileFile(const std::string& profile, const std::string& file, bool script);
  IWebViewObserver* ObserverOf(const std::string& profile);
  void EnsureCaretVisible();

  std::unique_ptr<WAYLANDHOST::CWaylandHost> m_host;
  mutable CCriticalSection m_lock;
  State m_state{State::ABSENT};
  bool m_respawned{false};
  IWebViewObserver* m_observer{nullptr};
  std::string m_observedProfile;
  bool m_wantPending{false};
  std::map<std::string, WAYLANDHOST::CWaylandSurface*> m_views;
  std::map<WAYLANDHOST::CWaylandSurface*, std::string> m_surfaces;
  bool m_keyboardOpen{false};
  bool m_textInputActive{false};
  WAYLANDHOST::TextInputState m_textInput;
  std::string m_keyboardText;
  uint32_t m_replaceBefore{0};
  uint32_t m_replaceAfter{0};
  int32_t m_coveredTop{0};
  int32_t m_scrolledCaretY{0};
};

/*!
 * \brief Shows Kodi's keyboard dialog for a page's text input on a job
 * thread, so neither the host nor the GUI thread waits on it
 */
class CWebKeyboardJob : public CJob
{
public:
  CWebKeyboardJob(CWebClientManager& manager, std::string text, bool hidden)
    : m_manager(manager),
      m_text(std::move(text)),
      m_hidden(hidden)
  {
  }

  bool DoWork() override;
  const char* GetType() const override { return "webkeyboard"; }

private:
  CWebClientManager& m_manager;
  std::string m_text;
  const bool m_hidden;
};

} // namespace WEB
} // namespace KODI
