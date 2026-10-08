if(TARGET ${APP_NAME_LC}::WaylandServer)
  set(PROTOCOLS linux-dmabuf-v1:stable/linux-dmabuf/linux-dmabuf-v1.xml
                presentation-time:stable/presentation-time/presentation-time.xml
                text-input-unstable-v3:unstable/text-input/text-input-unstable-v3.xml
                xdg-shell:stable/xdg-shell/xdg-shell.xml)

  set(SOURCES WaylandBuffer.cpp
              WaylandHost.cpp
              WaylandSurface.cpp)

  set(HEADERS WaylandBuffer.h
              WaylandHost.h
              WaylandSurface.h)

  foreach(protocol ${PROTOCOLS})
    string(REPLACE ":" ";" protocol ${protocol})
    list(GET protocol 0 name)
    list(GET protocol 1 xml)
    set(xml ${WAYLAND_PROTOCOLS_DIR}/${xml})
    set(header ${CMAKE_CURRENT_BINARY_DIR}/${name}-server-protocol.h)
    set(code ${CMAKE_CURRENT_BINARY_DIR}/${name}-protocol.c)
    add_custom_command(OUTPUT ${header}
                       COMMAND ${WAYLAND_SCANNER_EXECUTABLE} server-header ${xml} ${header}
                       DEPENDS ${xml}
                       COMMENT "Generating ${name} server header")
    add_custom_command(OUTPUT ${code}
                       COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${xml} ${code}
                       DEPENDS ${xml} ${header}
                       COMMENT "Generating ${name} protocol code")
    list(APPEND SOURCES ${code})
    list(APPEND HEADERS ${header})
  endforeach()

  include_directories(${CMAKE_CURRENT_BINARY_DIR})

  core_add_library(platform_linux_waylandhost)
endif()
