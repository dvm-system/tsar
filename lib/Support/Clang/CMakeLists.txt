set(SUPPORT_SOURCES Diagnostic.cpp Utils.cpp)

if(MSVC_IDE)
  file(GLOB SUPPORT_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Support/Clang/*.h)
  file(GLOB SUPPORT_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARSupportClang STATIC
  ${SUPPORT_SOURCES} ${SUPPORT_HEADERS} ${SUPPORT_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARSupportClang ${CLANG_LIBS} ${LLVM_LIBS})
endif()
add_dependencies(TSARSupportClang DirectivesGen DiagnosticKinds)
target_link_libraries(TSARSupportClang TSARSupport ${CLANG_LIBS} BCL::Core)

set_target_properties(TSARSupportClang PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)