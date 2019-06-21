set(TRANSFORM_SOURCES Passes.cpp DeadCodeElimination.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE TRANSFORM_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Transform/IR/*.h)
  file(GLOB_RECURSE TRANSFORM_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARTransformIR STATIC
  ${TRANSFORM_SOURCES} ${TRANSFORM_HEADERS} ${TRANSFORM_INTERNAL_HEADERS})

include_directories(${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR}/tsar)

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARTransformIR ${LLVM_LIBS})
endif()
target_link_libraries(TSARTransformIR ${LLVM_LIBS} BCL::Core)

set_target_properties(TSARTransformIR PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
