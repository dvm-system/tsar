set(ANALYSIS_SOURCES Passes.cpp DIMemoryMatcher.cpp NoMacroAssert.cpp
  MemoryMatcher.cpp LoopMatcher.cpp ExpressionMatcher.cpp CanonicalLoop.cpp
  PerfectLoop.cpp GlobalInfoExtractor.cpp ControlFlowTraits.cpp
  RegionDirectiveInfo.cpp VariableCollector.cpp ASTDependenceAnalysis.cpp
  IncludeTree.cpp Utils.cpp)


if(MSVC_IDE)
  file(GLOB_RECURSE ANALYSIS_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Analysis/Clang/*.h)
  file(GLOB_RECURSE ANALYSIS_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARAnalysisClang STATIC
  ${ANALYSIS_SOURCES} ${ANALYSIS_HEADERS} ${ANALYSIS_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARAnalysisClang ${CLANG_LIBS} ${LLVM_LIBS})
endif()
add_dependencies(TSARAnalysisClang DirectivesGen DiagnosticKinds
  IntrinsicsGen AttributesGen)
target_link_libraries(TSARAnalysisClang
  TSARAnalysisMemory TSARSupportClang TSARTool BCL::Core)

set_target_properties(TSARAnalysisClang PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
