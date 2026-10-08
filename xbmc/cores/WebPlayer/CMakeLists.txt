if(TARGET ${APP_NAME_LC}::WaylandServer)
  set(SOURCES WebClientManager.cpp
              WebPlayer.cpp
              WebVideoBuffer.cpp)

  set(HEADERS WebClientManager.h
              WebPlayer.h
              WebPlayerDefs.h
              WebVideoBuffer.h)

  include_directories(${CMAKE_BINARY_DIR}/${CORE_BUILD_DIR}/platform/linux/waylandhost)

  core_add_library(webplayer)
endif()
