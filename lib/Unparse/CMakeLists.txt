set(UNPARSE_SOURCES DIUnparser.cpp SourceUnparser.cpp SourceUnparserUtils.cpp
  Utils.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE UNPARSE_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Unparse/*.h)
  file(GLOB_RECURSE UNPARSE_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARUnparse STATIC
  ${UNPARSE_SOURCES} ${UNPARSE_HEADERS} ${UNPARSE_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARUnparse ${LLVM_LIBS})
endif()
target_link_libraries(TSARUnparse TSARAnalysisMemory BCL::Core)

set_target_properties(TSARUnparse PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

