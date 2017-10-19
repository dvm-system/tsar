configure_file("tsar_config.h.in" "tsar_config.h")
include_directories("${CMAKE_CURRENT_BINARY_DIR}")
set(TSAR_EXECUTABLES tsar_pass.cpp tsar_private.cpp tsar_dbg_output.cpp
  tsar_df_location.cpp tsar_utility.cpp tsar_action.cpp tsar_private_c.cpp
  tsar_transformation.cpp tsar_instrumentation.cpp tsar_tool.cpp tsar_test.cpp
  tsar_loop_matcher.cpp tsar_memory_matcher.cpp tsar_pragma.cpp
  DFRegionInfo.cpp DefinedMemory.cpp LiveMemory.cpp PerfectLoop.cpp
  ASTMergeAction.cpp EstimateMemory.cpp  AliasTreePrinter.cpp DIUnparser.cpp
  FrontendActions.cpp CanonicalLoop.cpp Intrinsics.cpp Instrumentation.cpp)
if (TSAR_SHARED)
  set(TSAR_EXECUTABLES ${TSAR_EXECUTABLES} Server.cpp PrivateServerPass.cpp)
else()
  set(TSAR_EXECUTABLES ${TSAR_EXECUTABLES} main.cpp)
endif()
if(MSVC_IDE)
  source_group(src FILES ${TSAR_EXECUTABLES})
  file(GLOB_RECURSE TSAR_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(src FILES ${TSAR_HEADERS})
  source_group(base FILES ${BASE_HEADERS})
  source_group(patches FILES ${TSAR_PATCHES})
endif()
if (TSAR_SHARED)
  add_library(tsar SHARED
    ${TSAR_EXECUTABLES} ${TSAR_HEADERS} ${BASE_HEADERS} ${TSAR_PATCHES})
  add_definitions("-DBCL_EXPORTING")
else()
  add_executable(tsar
    ${TSAR_EXECUTABLES} ${TSAR_HEADERS} ${BASE_HEADERS} ${TSAR_PATCHES})
endif()
tsar_tablegen(DiagnosticKinds.inc -gen-tsar-diags-defs
  SOURCE DiagnosticKinds.td
  TARGET DiagnosticKinds)
tsar_tablegen(Intrinsics.gen -gen-tsar-intrinsics-defs
  SOURCE Intrinsics.td
  TARGET IntrinsicsGen)
add_dependencies(tsar DiagnosticKinds IntrinsicsGen)
# Do not specify dependencies in case of PACKAGE_LLVM because Clang and LLVM
# libraries are not top-level targets in this case. So this works incorrect.
if(NOT PACKAGE_LLVM)
  add_dependencies(tsar ${CLANG_LIBS} ${LLVM_LIBS})
endif()
add_definitions("-D${PROJECT_NAME}_PROJECT" "-D${PROJECT_NAME}_CONFIG")
target_link_libraries(tsar ${CLANG_LIBS} ${LLVM_LIBS})
install(TARGETS tsar 
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION bin)
