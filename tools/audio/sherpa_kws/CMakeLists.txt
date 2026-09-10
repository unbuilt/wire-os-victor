cmake_minimum_required(VERSION 3.10)
project(sherpa_kws_trial LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(SHERPA_ROOT "" CACHE PATH "Pinned sherpa C API installation for the selected architecture")
if(NOT SHERPA_ROOT)
  message(FATAL_ERROR "Set SHERPA_ROOT to the matching host or target runtime installation")
endif()
find_path(SHERPA_INCLUDE sherpa-onnx/c-api/c-api.h PATHS "${SHERPA_ROOT}/include"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_library(SHERPA_LIBRARY sherpa-onnx-c-api PATHS "${SHERPA_ROOT}/lib"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT SHERPA_INCLUDE OR NOT SHERPA_LIBRARY)
  message(FATAL_ERROR "SHERPA_ROOT must contain the C API header and library")
endif()
add_executable(kws-replay kwsReplay.cpp)
target_include_directories(kws-replay PRIVATE "${SHERPA_INCLUDE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/util/source/anki")
target_link_libraries(kws-replay PRIVATE "${SHERPA_LIBRARY}")
target_compile_options(kws-replay PRIVATE -Wall -Wextra -Werror)
set_target_properties(kws-replay PROPERTIES BUILD_RPATH "${SHERPA_ROOT}/lib")
