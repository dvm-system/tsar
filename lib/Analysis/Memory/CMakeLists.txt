set(ANALYSIS_SOURCES Passes.cpp DependenceAnalysis.cpp PrivateAnalysis.cpp
  DIDependenceAnalysis.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE ANALYSIS_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Analysis/Memory/*.h)
  file(GLOB_RECURSE ANALYSIS_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARAnalysisMemory STATIC
  ${ANALYSIS_SOURCES} ${ANALYSIS_HEADERS} ${ANALYSIS_INTERNAL_HEADERS})

include_directories(${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR}/tsar)

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARAnalysisMemory ${LLVM_LIBS})
endif()
target_link_libraries(TSARAnalysisMemory ${LLVM_LIBS} BCL::Core)

set_target_properties(TSARAnalysisMemory PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
