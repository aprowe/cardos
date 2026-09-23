cmake_minimum_required(VERSION 3.20)
project(cardos C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if (MSVC)
  # C4310: CAPP_RGB truncates on purpose -- the truncation IS the byte swap.
  add_compile_options(/W4 /wd4127 /wd4310 /D_CRT_SECURE_NO_WARNINGS
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
  ${ROOT}/kernel/net/manifest.c   # update.c is device glue around it
  ${ROOT}/kernel/net/httpslot.c   # httpq.c is the task and lock around it
  ${ROOT}/kernel/net/dav.c
  ${ROOT}/kernel/input/mouse.c ${ROOT}/kernel/input/kbd_hid.c ${ROOT}/kernel/input/keyrepeat.c
  ${ROOT}/kernel/sys/input.c ${ROOT}/kernel/sys/rpc.c   # text in, commands in
  ${ROOT}/kernel/sys/hotkeys.c ${ROOT}/kernel/sys/chatlog.c ${ROOT}/kernel/sys/conf.c
  ${ROOT}/kernel/sys/logring.c   # applog.c is the device glue around it
  ${ROOT}/kernel/sys/tzreply.c   # tzlookup.c is the device glue around it
  ${ROOT}/kernel/sys/printdoc.c  # printq.c/btprint.c are the task and radio around it
  ${ROOT}/kernel/ui/pickmodel.c  # picker.c is the paint and fs glue
  ${ROOT}/kernel/ui/rect.c ${ROOT}/kernel/ui/wm.c)  # draw/desktop are device-only   # task.c is device glue; sched.c is the policy
add_executable(cardos_tests ${CARDOS_TEST_SOURCES} ${CARDOS_KERNEL_SOURCES})
target_include_directories(cardos_tests PRIVATE ${ROOT}/test ${ROOT})
# Host tests run the memory manager in paranoid mode: every allocation
# relocates unpinned blocks, so illegally retained pointers fail loudly.
target_compile_definitions(cardos_tests PRIVATE CARDOS_MEM_PARANOID)
enable_testing()
add_test(NAME unit COMMAND cardos_tests)
