if(TARGET ${APP_NAME_LC}::WaylandServer)
  set(SOURCES GUIDialogWebKeyboard.cpp
              GUIDialogWebOSD.cpp
              GUIWindowFullScreenWeb.cpp
              WebClientManager.cpp
              WebPlayer.cpp
              WebVideoBuffer.cpp)

  set(HEADERS GUIDialogWebKeyboard.h
              GUIDialogWebOSD.h
              GUIWindowFullScreenWeb.h
              WebClientManager.h
              WebPlayer.h
              WebPlayerDefs.h
              WebVideoBuffer.h)

  include_directories(${CMAKE_BINARY_DIR}/${CORE_BUILD_DIR}/platform/linux/waylandhost)

  core_add_library(webplayer)
endif()
