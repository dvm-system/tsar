set(ANALYSIS_SOURCES Passes.cpp DependenceAnalysis.cpp PrivateAnalysis.cpp
  DIDependenceAnalysis.cpp BitMemoryTrait.cpp ProcessTraitPass.cpp Utils.cpp
  TraitFilter.cpp NotInitializedMemory.cpp DIEstimateMemory.cpp
  EstimateMemory.cpp DefinedMemory.cpp LiveMemory.cpp AliasTreePrinter.cpp
  DIAliasTreePrinter.cpp DIMemoryLocation.cpp DFMemoryLocation.cpp
  Delinearization.cpp ServerUtils.cpp ClonedDIMemoryMatcher.cpp
  GlobalLiveMemory.cpp GlobalDefinedMemory.cpp DIClientServerInfo.cpp
  DIMemoryAnalysisServer.cpp DIArrayAccess.cpp AllocasModRef.cpp
  MemoryLocationRange.cpp GlobalsAccess.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE ANALYSIS_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Analysis/Memory/*.h)
  file(GLOB_RECURSE ANALYSIS_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARAnalysisMemory STATIC
  ${ANALYSIS_SOURCES} ${ANALYSIS_HEADERS} ${ANALYSIS_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARAnalysisMemory ${LLVM_LIBS})
endif()
add_dependencies(TSARAnalysisMemory IntrinsicsGen AttributesGen)
target_link_libraries(TSARAnalysisMemory TSARUnparse TSARAnalysis BCL::Core)

set_target_properties(TSARAnalysisMemory PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
