set(ANALYSIS_SOURCES Passes.cpp ParallelLoop.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE ANALYSIS_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Analysis/Parallel/*.h)
  file(GLOB_RECURSE ANALYSIS_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARAnalysisParallel STATIC
  ${ANALYSIS_SOURCES} ${ANALYSIS_HEADERS} ${ANALYSIS_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARAnalysisParallel ${LLVM_LIBS})
endif()
add_dependencies(TSARAnalysisParallel IntrinsicsGen AttributesGen)
target_link_libraries(TSARAnalysisParallel
  TSARUnparse TSARAnalysisMemory BCL::Core)

set_target_properties(TSARAnalysisParallel PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
