set(TRANSFORM_SOURCES Passes.cpp FormatPass.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE TRANSFORM_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Transform/AST/*.h)
  file(GLOB_RECURSE TRANSFORM_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARTransformAST STATIC
  ${TRANSFORM_SOURCES} ${TRANSFORM_HEADERS} ${TRANSFORM_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARTransformAST ${CLANG_LIBS} ${FLANG_LIBS} ${LLVM_LIBS})
endif()
add_dependencies(TSARTransformAST DirectivesGen DiagnosticKinds
  IntrinsicsGen AttributesGen)
target_link_libraries(TSARTransformAST PRIVATE
  TSARTransformClang TSARTool BCL::Core)
if (FLANG_FOUND)
  target_link_libraries(TSARTransformAST PRIVATE TSARTransformFlang)
endif()

set_target_properties(TSARTransformAST PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
