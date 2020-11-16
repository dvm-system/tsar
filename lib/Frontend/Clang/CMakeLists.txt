set(FRONTEND_SOURCES FrontendActions.cpp ASTMergeAction.cpp Passes.cpp
  Action.cpp PragmaHandlers.cpp Pragma.cpp TransformationContext.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE FRONTEND_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Frontend/Clang/*.h)
  file(GLOB_RECURSE FRONTEND_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARFrontendClang STATIC
  ${FRONTEND_SOURCES} ${FRONTEND_HEADERS} ${FRONTEND_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARFrontendClang ${CLANG_LIBS} ${FLANG_LIBS} ${LLVM_LIBS})
endif()
add_dependencies(TSARFrontendClang DirectivesGen DiagnosticKinds)
target_link_libraries(TSARFrontendClang TSARAnalysisClang BCL::Core ${FLANG_LIBS})

set_target_properties(TSARFrontendClang PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

