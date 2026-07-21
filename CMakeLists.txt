cmake_minimum_required(VERSION 3.16)
project(vibe-motion VERSION 0.1.0 LANGUAGES CXX)

option(VIBE_BUILD_TESTS "Build unit tests" ON)
option(VIBE_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(VIBE_ENABLE_SANITIZERS "Enable Clang AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(VIBE_ENABLE_NATIVE_OPTIMIZATION
  "Optimize for the CPU used to build vibe-motion (-march=native)" OFF)
option(VIBE_ENABLE_LTO "Enable Clang ThinLTO/interprocedural optimization" OFF)
set(VIBE_FFMPEG_ROOT "/usr/local" CACHE PATH "Root of one coherent FFmpeg installation")

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR
    "vibe-motion is a Clang-only project. Configure with "
    "-DCMAKE_TOOLCHAIN_FILE=cmake/clang-toolchain.cmake")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 22)
  message(FATAL_ERROR "vibe-motion requires Clang 22 or newer")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(VIBE_ENABLE_NATIVE_OPTIMIZATION)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" VIBE_NATIVE_OPTIMIZATION_SUPPORTED)
  if(NOT VIBE_NATIVE_OPTIMIZATION_SUPPORTED)
    message(FATAL_ERROR
      "VIBE_ENABLE_NATIVE_OPTIMIZATION was requested, but ${CMAKE_CXX_COMPILER} "
      "does not support -march=native")
  endif()
  add_compile_options(-march=native)
endif()

if(VIBE_ENABLE_LTO)
  if(VIBE_ENABLE_SANITIZERS)
    message(FATAL_ERROR
      "VIBE_ENABLE_LTO and VIBE_ENABLE_SANITIZERS cannot be enabled together. "
      "Use a separate non-sanitized build directory for LTO deployment.")
  endif()
  include(CheckIPOSupported)
  check_ipo_supported(RESULT VIBE_LTO_SUPPORTED OUTPUT VIBE_LTO_ERROR LANGUAGES CXX)
  if(NOT VIBE_LTO_SUPPORTED)
    message(FATAL_ERROR
      "VIBE_ENABLE_LTO was requested, but ThinLTO/IPO is not supported by the "
      "configured Clang toolchain:\n${VIBE_LTO_ERROR}")
  endif()
  # CMake selects Clang's -flto=thin implementation when IPO is enabled.
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(VIBE_ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined -fuse-ld=lld)
endif()

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format-22 clang-format REQUIRED)
find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy-22 clang-tidy REQUIRED)
if(VIBE_ENABLE_CLANG_TIDY)
  set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE}" "--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy")
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(LibXml2 REQUIRED)

set(VIBE_CLANG_WARNINGS
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wextra-semi
  -Wimplicit-fallthrough -Wnewline-eof -Werror)

find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
  HINTS "${VIBE_FFMPEG_ROOT}/include"
        "${VIBE_FFMPEG_ROOT}/include/${CMAKE_LIBRARY_ARCHITECTURE}"
  NO_DEFAULT_PATH)
foreach(component avformat avcodec avutil swscale)
  string(TOUPPER "${component}" upper_component)
  find_library(FFMPEG_${upper_component}_LIBRARY NAMES ${component}
    HINTS "${VIBE_FFMPEG_ROOT}/lib"
          "${VIBE_FFMPEG_ROOT}/lib64"
          "${VIBE_FFMPEG_ROOT}/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
    NO_DEFAULT_PATH)
  if(NOT FFMPEG_${upper_component}_LIBRARY)
    message(FATAL_ERROR "FFmpeg component ${component} was not found below ${VIBE_FFMPEG_ROOT}")
  endif()
endforeach()
if(NOT FFMPEG_INCLUDE_DIR)
  message(FATAL_ERROR "FFmpeg headers were not found below ${VIBE_FFMPEG_ROOT}")
endif()

add_library(vibe_motion_core
  src/config.cpp
  src/decode.cpp
  src/detection.cpp
  src/event.cpp
  src/media.cpp
  src/hooks.cpp
  src/http.cpp
  src/onvif.cpp
  src/runtime.cpp)
target_include_directories(vibe_motion_core PUBLIC include PRIVATE "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(vibe_motion_core PUBLIC Threads::Threads
  CURL::libcurl LibXml2::LibXml2
  "${FFMPEG_AVFORMAT_LIBRARY}" "${FFMPEG_AVCODEC_LIBRARY}"
  "${FFMPEG_AVUTIL_LIBRARY}" "${FFMPEG_SWSCALE_LIBRARY}")
target_compile_options(vibe_motion_core PRIVATE ${VIBE_CLANG_WARNINGS})
set_target_properties(vibe_motion_core PROPERTIES
  BUILD_RPATH "${VIBE_FFMPEG_ROOT}/lib;${VIBE_FFMPEG_ROOT}/lib64"
  INSTALL_RPATH "${VIBE_FFMPEG_ROOT}/lib;${VIBE_FFMPEG_ROOT}/lib64")

add_executable(vibe-motion src/main.cpp)
target_include_directories(vibe-motion PRIVATE "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(vibe-motion PRIVATE vibe_motion_core)
target_compile_options(vibe-motion PRIVATE ${VIBE_CLANG_WARNINGS})

install(TARGETS vibe-motion RUNTIME DESTINATION bin)
install(FILES packaging/vibe-motion.service DESTINATION lib/systemd/system)

file(GLOB_RECURSE VIBE_FORMAT_FILES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/include/*.hpp"
  "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.hpp"
  "${CMAKE_SOURCE_DIR}/tests/*.cpp")
file(GLOB VIBE_TIDY_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
add_custom_target(format
  COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${VIBE_FORMAT_FILES}
  COMMENT "Formatting C++ sources with clang-format 22")
add_custom_target(format-check
  COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${VIBE_FORMAT_FILES}
  COMMENT "Checking C++ formatting with clang-format 22")
add_custom_target(tidy
  COMMAND "${CLANG_TIDY_EXECUTABLE}" -p "${CMAKE_BINARY_DIR}" ${VIBE_TIDY_FILES}
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Running clang-tidy 22")

if(VIBE_BUILD_TESTS)
  enable_testing()
  foreach(test_name decode detection event config hooks http media onvif)
    add_executable(${test_name}_test tests/${test_name}_test.cpp)
    target_link_libraries(${test_name}_test PRIVATE vibe_motion_core)
    # The tests intentionally use assert() for checks and for calls whose
    # results are under test. Keep those expressions active in Release builds.
    target_compile_options(${test_name}_test PRIVATE ${VIBE_CLANG_WARNINGS} -UNDEBUG)
    add_test(NAME ${test_name} COMMAND ${test_name}_test)
  endforeach()
  add_dependencies(hooks_test vibe-motion)
  target_include_directories(media_test PRIVATE "${FFMPEG_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  find_program(FFMPEG_EXECUTABLE ffmpeg)
  if(FFMPEG_EXECUTABLE)
    add_test(NAME media_fixture COMMAND "${FFMPEG_EXECUTABLE}" -y -loglevel error
      -f lavfi -i testsrc2=size=160x120:rate=10 -t 3 -c:v libx264 -g 10 -pix_fmt yuv420p
      "${CMAKE_CURRENT_BINARY_DIR}/media-fixture.mp4")
    set_tests_properties(media PROPERTIES DEPENDS media_fixture)
    set_tests_properties(http PROPERTIES DEPENDS media_fixture)
  endif()
endif()
include(CTest)
