#
# lib/signalEssence/CMakeLists.txt
#

cmake_minimum_required(VERSION 3.10)

project(signal_essence_proj)

option(SE_V009 "Enable use of new Signal Essence v009 libs" ON)
option(SE_HIGHRES "Enable Signal Essence High Resolution Echo Cancelling" ON)

include(signalessence)

add_library(signal_essence STATIC
  aecExperiment.c
  "${SIGNALESSENCE_PROJ_SRC}/crossoverconfig.c"
  "${SIGNALESSENCE_PROJ_SRC}/fdsearchconfig.c"
  "${SIGNALESSENCE_PROJ_SRC}/mmif_proj.c"
  "${SIGNALESSENCE_PROJ_SRC}/policy_actions.c"
  "${SIGNALESSENCE_PROJ_SRC}/spatialfilterconfig.c"
  "${SIGNALESSENCE_VAD_SRC}/nfbin_f32_anki.c"
  "${SIGNALESSENCE_VAD_SRC}/svad.c"
)

target_link_libraries(signal_essence
  PRIVATE
  ${SIGNALESSENCE_LIBS}
)

target_include_directories(signal_essence
  PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  INTERFACE
  ${SIGNALESSENCE_INCLUDE_PATHS}
)

target_compile_definitions(signal_essence
  PRIVATE
  ANKI_AEC_EXPERIMENT_SUPPORTED=$<AND:$<BOOL:${VICOS}>,$<BOOL:${SE_V009}>,$<NOT:$<BOOL:${STANDALONE_SIM}>>>
  PUBLIC
  SE_V009=$<BOOL:${SE_V009}>
)

anki_build_target_license(signal_essence "ANKI" "Commercial")

set_source_files_properties(${SIGNALESSENCE_PROJ_SRC}/policy_actions.c PROPERTIES COMPILE_FLAGS "-Wno-shorten-64-to-32")
