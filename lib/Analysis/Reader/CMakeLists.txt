set(ANALYSIS_SOURCES Passes.cpp AnalysisReader.cpp  RegionWeights.cpp
  AnalysisWriter.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE ANALYSIS_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Analysis/Reader/*.h)
  file(GLOB_RECURSE ANALYSIS_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARAnalysisReader STATIC
  ${ANALYSIS_SOURCES} ${ANALYSIS_HEADERS} ${ANALYSIS_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARAnalysisReader ${LLVM_LIBS})
endif()
add_dependencies(TSARAnalysisReader IntrinsicsGen AttributesGen)
target_link_libraries(TSARAnalysisReader
  TSARAnalysis TSARAnalysisMemory TSARSupport BCL::Core)

set_target_properties(TSARAnalysisReader PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
