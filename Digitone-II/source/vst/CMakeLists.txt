# Il Digitone II come VST3 (DPF), per Mac e Windows: vst/README.md.
#
#   cmake -S vst -B BUILD -G Ninja -DDPF_ROOT=/percorso/DPF \
#         -DDTCORE_UNICORN_PREFIX=/percorso/unicorn [-DDNCORE_DSP_DIR=...] \
#         [-DCMAKE_TOOLCHAIN_FILE=mingw.cmake -DZLIB_ROOT=...]
#   cmake --build BUILD
#
# Mac: vst/scripts/build-mac.sh. Windows dal Mac: vst/scripts/build-windows.sh.
cmake_minimum_required(VERSION 3.20)
project(DigitoneVST C CXX)

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
# solo la libreria del motore: l'eseguibile di prova `dncore` non serve al plugin
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core dncore)
get_directory_property(_dn_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../core BUILDSYSTEM_TARGETS)
foreach(_t ${_dn_targets})
  get_target_property(_ty ${_t} TYPE)
  if(_ty STREQUAL "EXECUTABLE")
    set_target_properties(${_t} PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()
endforeach()

# gcc (mingw-w64): i builtin di clang che usa l'interprete del DSP (vst/compat-gcc.h)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(dncore PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/compat-gcc.h)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(LAYOUT_JSON ${CMAKE_CURRENT_SOURCE_DIR}/../core/layout/digitone2_panel.json)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/panel_layout.hpp
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-layout.py ${LAYOUT_JSON}
          ${CMAKE_CURRENT_BINARY_DIR}/panel_layout.hpp
  DEPENDS ${LAYOUT_JSON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-layout.py)
add_custom_target(panel_layout DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/panel_layout.hpp)

# Il runtime del DSP nativo (rt.h, periph.h, dn_dsp.*, dn_rtmain.c: codice nostro, non del firmware) incorporato
# come testo: NativeBuild.cpp lo scrive nella cartella dell'utente e lo compila con i sorgenti che traduce
# dal suo firmware (docs/DSP-INTERPRETE.md, "Costruzione sul computer dell'utente").
set(_dspdir ${CMAKE_CURRENT_SOURCE_DIR}/../core/dsp)
set(_embed_text "// generato da vst/CMakeLists.txt: non modificare\n#include \"NativeSrc.hpp\"\n")
foreach(_e "kSrcRtH|rt/rt.h" "kSrcPeriphH|rt/periph.h" "kSrcDnDspH|dn_dsp.h" "kSrcDnDspC|dn_dsp.c" "kSrcDnRtmainC|dn_rtmain.c")
  string(REPLACE "|" ";" _pair "${_e}")
  list(GET _pair 0 _var)
  list(GET _pair 1 _file)
  file(READ ${_dspdir}/${_file} _content)
  string(APPEND _embed_text "const char* const ${_var} = R\"DNRAW(${_content})DNRAW\";\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_dspdir}/${_file})
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/NativeSrc.cpp "${_embed_text}")

# VST3 ovunque; su macOS anche l'AUv2 (.component, per Logic Pro e GarageBand).
# -DWITH_AU=OFF per il solo VST3.
option(WITH_AU "AUv2 su macOS" ON)
set(EMU_PLUGIN_TARGETS vst3)
if(APPLE AND WITH_AU)
  list(APPEND EMU_PLUGIN_TARGETS au)
endif()

dpf_add_plugin(digitone2
  TARGETS ${EMU_PLUGIN_TARGETS}
  UI_TYPE opengl
  USE_FILE_BROWSER
  FILES_DSP DigitonePlugin.cpp NativeBuild.cpp ${CMAKE_CURRENT_BINARY_DIR}/NativeSrc.cpp
  FILES_UI DigitoneUI.cpp)

target_include_directories(digitone2 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(digitone2 PUBLIC dncore)
if(WIN32)
  target_link_libraries(digitone2 PUBLIC winhttp)       # il download del compilatore (NativeBuild.cpp)
endif()
add_dependencies(digitone2 panel_layout)

# strumenti di prova (non fanno parte del plugin; assenti nell'export per GitHub)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/rtref.c)
  # strumento di prova (non fa parte del plugin): il riferimento di vst3render, vst/tools/rtref.c
  add_executable(rtref EXCLUDE_FROM_ALL tools/rtref.c)
  target_link_libraries(rtref PRIVATE dncore)
  add_executable(drivetool EXCLUDE_FROM_ALL tools/drivetool.c)
  target_link_libraries(drivetool PRIVATE dncore)
  add_executable(nativetool EXCLUDE_FROM_ALL tools/nativetool.cpp NativeBuild.cpp ${CMAKE_CURRENT_BINARY_DIR}/NativeSrc.cpp)
  target_include_directories(nativetool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(nativetool PRIVATE dncore)
  if(WIN32)
    target_link_libraries(nativetool PRIVATE winhttp)
  endif()
  add_executable(fwtest EXCLUDE_FROM_ALL tools/fwtest.cpp)
  target_include_directories(fwtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(fwtest PRIVATE dncore)
endif()
