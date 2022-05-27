
set(FRONTEND_SOURCES TransformationContext.cpp Tooling.cpp Action.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE FRONTEND_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Frontend/Flang/*.h)
  file(GLOB_RECURSE FRONTEND_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARFrontendFlang STATIC
  ${FRONTEND_SOURCES} ${FRONTEND_HEADERS} ${FRONTEND_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARFrontendFlang ${FLANG_LIBS} ${LLVM_LIBS})
endif()
add_dependencies(TSARFrontendFlang DirectivesGen DiagnosticKinds)
target_link_libraries(TSARFrontendFlang BCL::Core)

set_target_properties(TSARFrontendFlang PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
