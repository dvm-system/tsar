set(ANALYSIS_SOURCES Passes.cpp ExpressionMatcher.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE ANALYSIS_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Analysis/Flang/*.h)
  file(GLOB_RECURSE ANALYSIS_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARAnalysisFlang STATIC
  ${ANALYSIS_SOURCES} ${ANALYSIS_HEADERS} ${ANALYSIS_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARAnalysisFlang ${FLANG_LIBS} ${LLVM_LIBS})
endif()
add_dependencies(TSARAnalysisFlang DirectivesGen DiagnosticKinds
  IntrinsicsGen AttributesGen)
target_link_libraries(TSARAnalysisFlang
  TSARAnalysisMemory TSARSupportFlang TSARTool BCL::Core)

set_target_properties(TSARAnalysisFlang PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
