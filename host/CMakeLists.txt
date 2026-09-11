cmake_minimum_required(VERSION 3.20)
project(cardos C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if (MSVC)
  add_compile_options(/W4 /wd4127 /D_CRT_SECURE_NO_WARNINGS
    /we4013)  # implicit function declaration -> error, as gcc does on device
else()
  add_compile_options(-Wall -Wextra)
endif()
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
file(GLOB CARDOS_TEST_SOURCES CONFIGURE_DEPENDS ${ROOT}/test/*.c)
# Only the portable modules build on the host; kernel/drv is device-only.
file(GLOB_RECURSE CARDOS_KERNEL_SOURCES CONFIGURE_DEPENDS
  ${ROOT}/kernel/mem/*.c ${ROOT}/kernel/swap/*.c
  ${ROOT}/kernel/app/appimage.c   # launcher.c is device-only (ESP-IDF)
  ${ROOT}/kernel/task/sched.c
  ${ROOT}/kernel/fs/path.c
  ${ROOT}/kernel/input/mouse.c
  ${ROOT}/kernel/ui/rect.c ${ROOT}/kernel/ui/wm.c)  # draw/desktop are device-only   # task.c is device glue; sched.c is the policy
add_executable(cardos_tests ${CARDOS_TEST_SOURCES} ${CARDOS_KERNEL_SOURCES})
target_include_directories(cardos_tests PRIVATE ${ROOT}/test ${ROOT}/kernel)
# Host tests run the memory manager in paranoid mode: every allocation
# relocates unpinned blocks, so illegally retained pointers fail loudly.
target_compile_definitions(cardos_tests PRIVATE CARDOS_MEM_PARANOID)
enable_testing()
add_test(NAME unit COMMAND cardos_tests)
