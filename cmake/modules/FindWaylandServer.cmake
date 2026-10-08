#.rst:
# FindWaylandServer
# -----------------
# Finds the wayland-server library, the wayland-scanner code generator and
# the wayland-protocols XML files needed to host Wayland clients
#
# This will define the following target:
#
#   ${APP_NAME_LC}::WaylandServer   - The wayland-server library
#
# and the following variables:
#
#   WAYLAND_SCANNER_EXECUTABLE   - wayland-scanner program
#   WAYLAND_PROTOCOLS_DIR        - directory containing the wayland-protocols XML files

if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})

  include(cmake/scripts/common/ModuleHelpers.cmake)

  set(${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC wayland-server)
  set(${${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC}_DISABLE_VERSION ON)

  SETUP_BUILD_VARS()

  SETUP_FIND_SPECS()

  SEARCH_EXISTING_PACKAGES()

  if(${${CMAKE_FIND_PACKAGE_NAME}_SEARCH_NAME}_FOUND)
    find_package(PkgConfig ${SEARCH_QUIET})

    pkg_check_modules(PC_WAYLAND_SCANNER wayland-scanner ${SEARCH_QUIET})
    if(PC_WAYLAND_SCANNER_FOUND)
      pkg_get_variable(PC_WAYLAND_SCANNER_EXECUTABLE wayland-scanner wayland_scanner)
    endif()
    find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner
                                            HINTS ${PC_WAYLAND_SCANNER_EXECUTABLE})

    pkg_check_modules(PC_WAYLAND_PROTOCOLS wayland-protocols ${SEARCH_QUIET})
    if(PC_WAYLAND_PROTOCOLS_FOUND)
      pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    endif()
    set(WAYLAND_PROTOCOLS_DIR ${WAYLAND_PROTOCOLS_DIR} CACHE INTERNAL "")

    if(WAYLAND_SCANNER_EXECUTABLE AND WAYLAND_PROTOCOLS_DIR)
      add_library(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} ALIAS PkgConfig::${${CMAKE_FIND_PACKAGE_NAME}_SEARCH_NAME})

      set(${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_COMPILE_DEFINITIONS HAVE_WAYLAND_SERVER)

      ADD_TARGET_COMPILE_DEFINITION()
    elseif(WaylandServer_FIND_REQUIRED)
      message(FATAL_ERROR "wayland-scanner or wayland-protocols were not found.")
    endif()
  else()
    if(WaylandServer_FIND_REQUIRED)
      message(FATAL_ERROR "wayland-server library was not found.")
    endif()
  endif()
endif()
