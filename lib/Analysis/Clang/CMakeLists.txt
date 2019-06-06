set(ANALYSIS_SOURCES Passes.cpp DIMemoryMatcher.cpp NoMacroAssert.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE ANALYSIS_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Analysis/Clang/*.h)
  file(GLOB_RECURSE ANALYSIS_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARClangAnalysis STATIC
  ${ANALYSIS_SOURCES} ${ANALYSIS_HEADERS} ${ANALYSIS_INTERNAL_HEADERS})

include_directories(${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR}/tsar)

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARClangAnalysis ${CLANG_LIBS} ${LLVM_LIBS})
endif()
target_link_libraries(TSARClangAnalysis ${CLANG_LIBS} ${LLVM_LIBS} BCL::Core)

set_target_properties(TSARClangAnalysis PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
