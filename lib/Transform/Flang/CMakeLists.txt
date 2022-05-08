set(TRANSFORM_SOURCES Passes.cpp Format.cpp ConstantReplacement.cpp
  VariableRegistration.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE TRANSFORM_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Transform/Flang/*.h)
  file(GLOB_RECURSE TRANSFORM_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARTransformFlang STATIC
  ${TRANSFORM_SOURCES} ${TRANSFORM_HEADERS} ${TRANSFORM_INTERNAL_HEADERS})

if(MSVC)
  target_compile_options(TSARTransformFlang
    PRIVATE $<$<NOT:$<CONFIG:Release>>:/bigobj>)
endif()

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARTransformFlang ${FLANG_LIBS} ${LLVM_LIBS})
endif()
add_dependencies(TSARTransformFlang DirectivesGen DiagnosticKinds
  IntrinsicsGen AttributesGen)
target_link_libraries(TSARTransformFlang TSARTool BCL::Core)

set_target_properties(TSARTransformFlang PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
