set(TRANSFORM_SOURCES Passes.cpp DeadCodeElimination.cpp InterprocAttr.cpp
  MetadataUtils.cpp Utils.cpp CallExtractor.cpp DependenceInliner.cpp
  NoCaptureAnalysis.cpp PointerScalarizer.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE TRANSFORM_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Transform/IR/*.h)
  file(GLOB_RECURSE TRANSFORM_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARTransformIR STATIC
  ${TRANSFORM_SOURCES} ${TRANSFORM_HEADERS} ${TRANSFORM_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARTransformIR ${LLVM_LIBS})
endif()
add_dependencies(TSARTransformIR TSARAnalysisMemory IntrinsicsGen AttributesGen)
target_link_libraries(TSARTransformIR BCL::Core)

set_target_properties(TSARTransformIR PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
