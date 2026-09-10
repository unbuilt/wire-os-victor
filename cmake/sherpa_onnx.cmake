if (NOT VICOS)
  message(FATAL_ERROR "SHERPA_KWS currently requires the VICOS ARM softfp runtime")
endif()

get_filename_component(_sherpa_work "${CMAKE_SOURCE_DIR}/../../_build/sherpa-kws" ABSOLUTE)
set(SHERPA_ONNX_ROOT "${_sherpa_work}/target" CACHE PATH "Prepared ARM sherpa-onnx runtime")
set(SHERPA_KWS_MODEL_DIR
  "${_sherpa_work}/sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01"
  CACHE PATH "Prepared Wenetspeech keyword model")
set(SHERPA_KWS_LICENSE_DIR "${_sherpa_work}/ota-licenses"
  CACHE PATH "Prepared sherpa-onnx and ONNX Runtime license notices")

if (NOT EXISTS "${SHERPA_ONNX_ROOT}/include/sherpa-onnx/c-api/c-api.h")
  message(FATAL_ERROR "Prepare the target runtime with tools/audio/sherpa_kws/prepare_runtime.sh target")
endif()

add_library(sherpa_onnx_runtime SHARED IMPORTED GLOBAL)
set_target_properties(sherpa_onnx_runtime PROPERTIES
  IMPORTED_LOCATION "${SHERPA_ONNX_ROOT}/lib/libonnxruntime.so.1.17.1")
add_library(sherpa_onnx_c_api SHARED IMPORTED GLOBAL)
set_target_properties(sherpa_onnx_c_api PROPERTIES
  IMPORTED_LOCATION "${SHERPA_ONNX_ROOT}/lib/libsherpa-onnx-c-api.so"
  INTERFACE_INCLUDE_DIRECTORIES "${SHERPA_ONNX_ROOT}/include"
  INTERFACE_LINK_LIBRARIES sherpa_onnx_runtime)

function(sherpa_kws_copy source destination)
  if (NOT EXISTS "${source}")
    message(FATAL_ERROR "Missing sherpa KWS packaging input: ${source}; run build_ota.sh prepare")
  endif()
  get_filename_component(directory "${destination}" DIRECTORY)
  add_custom_command(OUTPUT "${destination}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${source}" "${destination}"
    DEPENDS "${source}"
    VERBATIM)
  set(_sherpa_outputs ${_sherpa_outputs} "${destination}" PARENT_SCOPE)
endfunction()

set(_sherpa_outputs "")
foreach(lib sherpa_onnx_c_api sherpa_onnx_runtime)
  get_target_property(source ${lib} IMPORTED_LOCATION)
  get_filename_component(name "${source}" NAME)
  sherpa_kws_copy("${source}" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${name}")
endforeach()

set(_sherpa_assets "${CMAKE_BINARY_DIR}/data/assets/cozmo_resources/assets/sherpaKws")
foreach(part encoder decoder joiner)
  set(name "${part}-epoch-12-avg-2-chunk-16-left-64.int8.onnx")
  sherpa_kws_copy("${SHERPA_KWS_MODEL_DIR}/${name}" "${_sherpa_assets}/${name}")
endforeach()
sherpa_kws_copy("${SHERPA_KWS_MODEL_DIR}/tokens.txt" "${_sherpa_assets}/tokens.txt")
sherpa_kws_copy("${CMAKE_SOURCE_DIR}/tools/audio/sherpa_kws/keywords.txt"
  "${_sherpa_assets}/keywords.txt")
foreach(name SHERPA_ONNX_LICENSE ONNXRUNTIME_LICENSE ONNXRUNTIME_THIRD_PARTY_NOTICES
             SHERPA_ONNX_THIRD_PARTY_NOTICES)
  sherpa_kws_copy("${SHERPA_KWS_LICENSE_DIR}/${name}"
    "${CMAKE_BINARY_DIR}/data/licenses/sherpaKws/${name}")
endforeach()
add_custom_target(copy_sherpa_kws ALL DEPENDS ${_sherpa_outputs})
anki_build_target_license(sherpa_onnx_c_api "Apache-2.0,${SHERPA_KWS_LICENSE_DIR}/SHERPA_ONNX_LICENSE")
anki_build_target_license(sherpa_onnx_runtime "MIT,${SHERPA_KWS_LICENSE_DIR}/ONNXRUNTIME_LICENSE")
