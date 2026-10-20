# Il Digitakt II come VST3 (DPF), per Mac e Windows: vst/README.md.
#
#   cmake -S vst -B BUILD -G Ninja -DDPF_ROOT=/percorso/DPF \
#         -DDTCORE_UNICORN_PREFIX=/percorso/unicorn [-DCMAKE_TOOLCHAIN_FILE=mingw.cmake -DZLIB_ROOT=...]
#   cmake --build BUILD
#
# Per Windows dal Mac c'e' tutto in vst/scripts/build-windows.sh.
cmake_minimum_required(VERSION 3.20)
project(DigitaktVST C CXX)

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
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core dtcore)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(LAYOUT_JSON ${CMAKE_CURRENT_SOURCE_DIR}/../core/layout/digitakt2_panel.json)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/panel_layout.hpp
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-layout.py ${LAYOUT_JSON}
          ${CMAKE_CURRENT_BINARY_DIR}/panel_layout.hpp
  DEPENDS ${LAYOUT_JSON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-layout.py)
add_custom_target(panel_layout DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/panel_layout.hpp)

# VST3 ovunque; su macOS anche l'AUv2 (.component, per Logic Pro e GarageBand).
# -DWITH_AU=OFF per il solo VST3.
option(WITH_AU "AUv2 su macOS" ON)
set(EMU_PLUGIN_TARGETS vst3)
if(APPLE AND WITH_AU)
  list(APPEND EMU_PLUGIN_TARGETS au)
endif()

dpf_add_plugin(digitakt2
  TARGETS ${EMU_PLUGIN_TARGETS}
  UI_TYPE opengl
  USE_FILE_BROWSER
  FILES_DSP DigitaktPlugin.cpp
  FILES_UI DigitaktUI.cpp)

target_include_directories(digitakt2 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../core           # machines/digitakt2/profiles.inc.c: i firmware accettati
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/src)
target_link_libraries(digitakt2 PUBLIC dtcore)
add_dependencies(digitakt2 panel_layout)

# l'AUv2 ha nome e codice suoi (DistrhoPluginInfo.h): anche lo strumento che
# scrive l'Info.plist del .component deve vederli
if(TARGET digitakt2-au)
  target_compile_definitions(digitakt2-au PRIVATE DIGITAKT_AU2=1)
  target_compile_definitions(digitakt2-export PRIVATE DIGITAKT_AU2=1)
endif()
