cmake_minimum_required(VERSION 3.16.0)

# The firmware's version (esp_app_desc, shown by bootinfo, About, the boot
# banner and `update version`) is `git describe`, which ESP-IDF reads when
# CMake configures -- and an ordinary build does not configure again, so a
# build after a commit or a tag went on saying the old version (a device said
# v0.10.0 running v0.11.0; a build after v0.15.1 said v0.14.0-21). Making the
# configure depend on git's HEAD, index and refs runs it again after any of
# those changes.
execute_process(COMMAND git rev-parse --absolute-git-dir
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE CARDOS_GIT_DIR OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(CARDOS_GIT_DIR)
  execute_process(COMMAND git rev-parse --git-common-dir
                  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                  OUTPUT_VARIABLE CARDOS_GIT_COMMON OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  get_filename_component(CARDOS_GIT_COMMON "${CARDOS_GIT_COMMON}" ABSOLUTE
                         BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  foreach(f "${CARDOS_GIT_DIR}/HEAD" "${CARDOS_GIT_DIR}/index"
            "${CARDOS_GIT_COMMON}/refs/heads" "${CARDOS_GIT_COMMON}/refs/tags"
            "${CARDOS_GIT_COMMON}/packed-refs")
    if(EXISTS "${f}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${f}")
    endif()
  endforeach()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cardos)
