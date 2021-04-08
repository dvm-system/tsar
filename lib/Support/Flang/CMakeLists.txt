set(SUPPORT_SOURCES Rewriter.cpp Diagnostic.cpp)

if(MSVC_IDE)
  file(GLOB SUPPORT_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Support/Flang/*.h)
  file(GLOB SUPPORT_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARSupportFlang STATIC
  ${SUPPORT_SOURCES} ${SUPPORT_HEADERS} ${SUPPORT_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARSupportFlang ${FLANG_LIBS} ${LLVM_LIBS})
endif()
add_dependencies(TSARSupportFlang DirectivesGen DiagnosticKinds)
target_link_libraries(TSARSupportFlang TSARSupport BCL::Core)

set_target_properties(TSARSupportFlang PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)