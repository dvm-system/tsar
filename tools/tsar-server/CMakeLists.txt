set(TSAR_SHARED_SOURCES Server.cpp PrivateServerPass.cpp ClangMessages.cpp)

if(MSVC_IDE)
  file(GLOB TSAR_SHARED_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
  if (APC_FOUND)
    source_group(apc FILES ${APC_CORE_HEADERS})
  endif()
  if(lp_solve_FOUND)
    source_group(lp_solve FILES ${lp_solve_HEADERS})
  endif()
endif()

set_property(TARGET TSARTool TSARSupport TSARAnalysis TSARAnalysisReader
  TSARAnalysisMemory TSARAnalysisParallel TSARTransformIR TSARTransformMixed
  TSARTransformClang TSARAnalysisClang TSARFrontendClang TSARSupportClang
  TSARUnparse PROPERTY POSITION_INDEPENDENT_CODE ON)

add_library(TSARServer SHARED
  ${TSAR_SHARED_SOURCES} ${TSAR_SHARED_INTERNAL_HEADERS})

add_dependencies(TSARServer TSARTool)
if(NOT PACKAGE_LLVM)
  add_dependencies(TSARServer ${CLANG_LIBS} ${LLVM_LIBS})
endif()
target_link_libraries(TSARServer TSARTool ${CLANG_LIBS} ${LLVM_LIBS} BCL::Core)

set_target_properties(TSARServer PROPERTIES
  COMPILE_DEFINITIONS BCL_EXPORTING
  FOLDER "${TSAR_SERVER_FOLDER}")

install(TARGETS TSARServer
  LIBRARY DESTINATION shared
  RUNTIME DESTINATION shared
  ARCHIVE DESTINATION lib)