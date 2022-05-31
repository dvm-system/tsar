set(ANALYSIS_SOURCES Passes.cpp PrintUtils.cpp DFRegionInfo.cpp Attributes.cpp
  Intrinsics.cpp AnalysisSocket.cpp AnalysisServer.cpp)

if(MSVC_IDE)
  file(GLOB ANALYSIS_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Analysis/*.h)
  file(GLOB ANALYSIS_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARAnalysis STATIC
  ${ANALYSIS_SOURCES} ${ANALYSIS_HEADERS} ${ANALYSIS_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARAnalysis ${LLVM_LIBS})
endif()
target_link_libraries(TSARAnalysis TSARSupport BCL::Core)

set_target_properties(TSARAnalysis PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

include(tsar-tablegen)

tsar_tablegen(Intrinsics.gen -gen-tsar-intrinsics-defs
  SOURCE ${PROJECT_SOURCE_DIR}/include/tsar/Analysis/Intrinsics.td
  TARGET IntrinsicsGen)
tsar_tablegen(Attributes.gen -gen-tsar-attributes-defs
  SOURCE ${PROJECT_SOURCE_DIR}/include/tsar/Analysis/Attributes.td
  TARGET AttributesGen)
add_dependencies(TSARAnalysis IntrinsicsGen AttributesGen)

add_subdirectory(Clang)
if(FLANG_FOUND)
  add_subdirectory(Flang)
endif()
add_subdirectory(Memory)
add_subdirectory(Reader)
add_subdirectory(Parallel)
