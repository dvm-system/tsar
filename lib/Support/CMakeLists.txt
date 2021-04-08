set(SUPPORT_SOURCES SCEVUtils.cpp GlobalOptions.cpp Utils.cpp Directives.cpp
  PassBarrier.cpp EmptyPass.cpp Diagnostic.cpp RewriterBase.cpp)

if(MSVC_IDE)
  file(GLOB SUPPORT_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Support/*.h)
  file(GLOB SUPPORT_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARSupport STATIC
  ${SUPPORT_SOURCES} ${SUPPORT_HEADERS} ${SUPPORT_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARSupport ${LLVM_LIBS})
endif()
target_link_libraries(TSARSupport ${LLVM_LIBS} BCL::Core)

set_target_properties(TSARSupport PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

include(tsar-tablegen)

tsar_tablegen(Directives.gen -gen-tsar-directives-defs
  SOURCE ${PROJECT_SOURCE_DIR}/include/tsar/Support/Directives.td
  TARGET DirectivesGen)
tsar_tablegen(DiagnosticKinds.inc -gen-tsar-diags-defs
  SOURCE ${PROJECT_SOURCE_DIR}/include/tsar/Support/DiagnosticKinds.td
  TARGET DiagnosticKinds)
add_dependencies(TSARSupport DirectivesGen DiagnosticKinds)

add_subdirectory(Clang)

if(FLANG_FOUND)
  add_subdirectory(Flang)
endif()