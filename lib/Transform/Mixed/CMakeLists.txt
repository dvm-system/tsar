set(TRANSFORM_SOURCES Passes.cpp Instrumentation.cpp DILoopRetriever.cpp
  DINodeRetriever.cpp)

if(FLANG_FOUND)
  set(TRANSFORM_SOURCES ${TRANSFORM_SOURCES} DummyScopeAAPass.cpp
    DIVariableRetriever.cpp)
else()
  set(TRANSFORM_SOURCES ${TRANSFORM_SOURCES} FlangStubs.cpp)
endif()

if(MSVC_IDE)
  file(GLOB_RECURSE TRANSFORM_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Transform/Mixed/*.h)
  file(GLOB_RECURSE TRANSFORM_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARTransformMixed STATIC
  ${TRANSFORM_SOURCES} ${TRANSFORM_HEADERS} ${TRANSFORM_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARTransformMixed ${LLVM_LIBS} ${CLANG_LIBS})
endif()
add_dependencies(TSARTransformMixed IntrinsicsGen AttributesGen)
target_link_libraries(TSARTransformMixed TSARTransformIR BCL::Core)

set_target_properties(TSARTransformMixed PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
