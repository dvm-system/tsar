configure_file(${PROJECT_SOURCE_DIR}/include/tsar/Core/tsar-config.h.in
  tsar-config.h)

set(CORE_SOURCES TransformationContext.cpp Query.cpp Passes.cpp Tool.cpp
  IRAction.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE CORE_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Core/*.h)
  file(GLOB_RECURSE CORE_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
  if (APC_FOUND)
    source_group(apc FILES ${APC_CORE_HEADERS})
  endif()
  if(lp_solve_FOUND)
    source_group(lp_solve FILES ${lp_solve_HEADERS})
  endif()
endif()

add_library(TSARTool STATIC
  ${CORE_SOURCES} ${CORE_HEADERS} ${CORE_INTERNAL_HEADERS})

set_target_properties(TSARTool PROPERTIES FOLDER "${TSAR_LIBRARY_FOLDER}")
set_target_properties(TSARTool PROPERTIES
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

if(APC_FOUND)
   add_dependencies(TSARTool APC)
   target_link_libraries(TSARTool PRIVATE APC)
endif()

if(lp_solve_FOUND)
  add_dependencies(TSARTool lp_solve)
  target_link_libraries(TSARTool PRIVATE lp_solve)
endif()

# Do not specify dependencies in case of PACKAGE_LLVM because Clang and LLVM
# libraries are not top-level targets in this case. So this works incorrect.
if(NOT PACKAGE_LLVM)
  if(Flang_FOUND)
    add_dependencies(TSARTool TSARTransformFlang TSARFrontendFlang
      TSARAnalysisFlang TSARSupportFlang)
  endif()
  add_dependencies(TSARTool
    TSARTransformAST
    TSARTransformClang TSARTransformMixed TSARTransformIR TSARFrontendClang
    TSARAnalysisClang TSARAnalysisParallel TSARAnalysisReader TSARAnalysisMemory
    TSARAnalysis TSARUnparse TSARSupportClang TSARSupport
    ${CLANG_LIBS} ${LLVM_LIBS} ${FLANG_LIBS} BCL::Core)
endif()

if (FLANG_FOUND)
  target_link_libraries(TSARTool PRIVATE TSARTransformFlang TSARFrontendFlang
    TSARAnalysisFlang TSARSupportFlang)
endif()

target_link_libraries(TSARTool PRIVATE
  TSARTransformAST
  TSARTransformClang TSARTransformMixed TSARTransformIR TSARFrontendClang
  TSARAnalysisClang TSARAnalysisParallel TSARAnalysisReader TSARAnalysisMemory
  TSARAnalysis TSARUnparse TSARSupportClang TSARSupport BCL::Core)

if(WIN32)
  # clangDriver uses version.dll.
  target_link_libraries(TSARTool PRIVATE version)
endif()
