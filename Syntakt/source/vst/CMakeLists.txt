# La Syntakt come VST3 (DPF), per Mac e Windows: vst/README.md.
#
#   cmake -S vst -B BUILD -G Ninja -DDPF_ROOT=/percorso/DPF -DDTCORE_DIR=/percorso/dtcore \
#         -DDTCORE_UNICORN_PREFIX=/percorso/unicorn [-DCMAKE_TOOLCHAIN_FILE=mingw.cmake -DZLIB_ROOT=...]
#   cmake --build BUILD
#
# DTCORE_DIR e' il motore comune del Digitakt II (digitakt/emulator/core) a un
# commit fisso: gli script lo esportano con `git archive` (vst/scripts/dtcore.sh),
# cosi' il lavoro in corso sul Digitakt non cambia questa build.
# Per Mac e Windows c'e' tutto in vst/scripts/build-mac.sh e build-windows.sh.
cmake_minimum_required(VERSION 3.20)
project(SyntaktVST C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(DPF_ROOT "" CACHE PATH "sorgenti di DPF (https://github.com/DISTRHO/DPF)")
if(NOT DPF_ROOT)
  message(FATAL_ERROR "DPF_ROOT non impostato")
endif()

add_subdirectory(${DPF_ROOT} dpf)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core stcore)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(LAYOUT_JSON ${CMAKE_CURRENT_SOURCE_DIR}/../core/layout/syntakt_panel.json)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/panel_layout.hpp
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-layout.py ${LAYOUT_JSON}
          ${CMAKE_CURRENT_BINARY_DIR}/panel_layout.hpp
  DEPENDS ${LAYOUT_JSON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-layout.py)
add_custom_target(panel_layout DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/panel_layout.hpp)

set(PLUGIN_DSP_FILES SyntaktPlugin.cpp)
# finche' stcore.h non ha st_coldboot_session (core/src/st_boot.inc.c) il primo
# avvio risponde "non disponibile": ColdBootStub.cpp
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../core/include/stcore.h STCORE_H)
string(FIND "${STCORE_H}" "st_coldboot_session" HAVE_COLDBOOT)
if(HAVE_COLDBOOT EQUAL -1)
  message(STATUS "stcore: st_coldboot_session assente, il primo avvio e' un finto")
  list(APPEND PLUGIN_DSP_FILES ColdBootStub.cpp)
endif()

# VST3 ovunque; su macOS anche l'AUv2 (.component, per Logic Pro e GarageBand).
# -DWITH_AU=OFF per il solo VST3.
option(WITH_AU "AUv2 su macOS" ON)
set(EMU_PLUGIN_TARGETS vst3)
if(APPLE AND WITH_AU)
  list(APPEND EMU_PLUGIN_TARGETS au)
endif()

dpf_add_plugin(syntakt
  TARGETS ${EMU_PLUGIN_TARGETS}
  UI_TYPE opengl
  USE_FILE_BROWSER
  FILES_DSP ${PLUGIN_DSP_FILES}
  FILES_UI SyntaktUI.cpp)

target_include_directories(syntakt PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
  ${DTCORE_DIR}/include
  ${DTCORE_DIR}/src)
target_link_libraries(syntakt PUBLIC stcore)
add_dependencies(syntakt panel_layout)
