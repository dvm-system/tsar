configure_file("tsar_config.h.in" "tsar_config.h")
include_directories("${CMAKE_CURRENT_BINARY_DIR}")
set(TSAR_SOURCES tsar_pass.cpp tsar_private.cpp tsar_dbg_output.cpp
  tsar_df_location.cpp tsar_utility.cpp tsar_action.cpp
  tsar_transformation.cpp tsar_tool.cpp tsar_test.cpp
  tsar_loop_matcher.cpp tsar_memory_matcher.cpp tsar_pragma.cpp
  DFRegionInfo.cpp DefinedMemory.cpp LiveMemory.cpp PerfectLoop.cpp
  ASTMergeAction.cpp EstimateMemory.cpp  AliasTreePrinter.cpp DIUnparser.cpp
  FrontendActions.cpp CanonicalLoop.cpp Intrinsics.cpp Instrumentation.cpp
  MetadataUtils.cpp tsar_finliner.cpp tsar_fcopy_elimination.cpp Diagnostic.cpp
  Directives.cpp ClangUtils.cpp GlobalInfoExtractor.cpp ClangFormatPass.cpp
  NoMacroAssert.cpp DILoopRetriever.cpp DIMemoryLocation.cpp
  SourceUnparser.cpp SourceUnparserUtils.cpp DIEstimateMemory.cpp
  DIAliasTreePrinter.cpp DIDependencyAnalysis.cpp RenameLocal.cpp
  DeadDeclsElimination.cpp DIGlobalRetriever.cpp InterprocAttr.cpp
  CalleeProcLocation.cpp Attributes.cpp)
if(MSVC_IDE)
  source_group(src FILES ${TSAR_SOURCES})
  file(GLOB_RECURSE TSAR_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(src FILES ${TSAR_HEADERS})
  source_group(bcl FILES ${BCL_CORE_HEADERS})
  source_group(patches FILES ${TSAR_PATCHES})
  if(APC_FOUND)
    source_group(apc FILES ${APC_CORE_HEADERS})
  endif()
  if(lp_solve_FOUND)
    source_group(lp_solve FILES ${lp_solve_HEADERS})
  endif()
endif()
add_library(TSARCore STATIC ${TSAR_SOURCES} ${TSAR_HEADERS} ${TSAR_PATCHES})
set_target_properties(TSARCore PROPERTIES FOLDER "${TSAR_LIBRARY_FOLDER}")
include(tsar-tablegen)
tsar_tablegen(DiagnosticKinds.inc -gen-tsar-diags-defs
  SOURCE DiagnosticKinds.td
  TARGET DiagnosticKinds)
tsar_tablegen(Intrinsics.gen -gen-tsar-intrinsics-defs
  SOURCE Intrinsics.td
  TARGET IntrinsicsGen)
tsar_tablegen(Attributes.gen -gen-tsar-attributes-defs
  SOURCE Attributes.td
  TARGET AttributesGen)
tsar_tablegen(Directives.gen -gen-tsar-directives-defs
  SOURCE Directives.td
  TARGET DirectivesGen)
set_target_properties(${TSAR_TABLEGEN} PROPERTIES FOLDER "Tablegenning")
set_target_properties(TSARCore PROPERTIES
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
add_dependencies(TSARCore
  DiagnosticKinds IntrinsicsGen AttributesGen DirectivesGen)
# Do not specify dependencies in case of PACKAGE_LLVM because Clang and LLVM
# libraries are not top-level targets in this case. So this works incorrect.
if(NOT PACKAGE_LLVM)
  add_dependencies(TSARCore ${CLANG_LIBS} ${LLVM_LIBS})
endif()
target_link_libraries(TSARCore ${CLANG_LIBS} ${LLVM_LIBS} BCL::Core)
if(APC_FOUND)
  target_link_libraries(TSARCore APC::APCCore)
  add_definitions("-D__SPC")
endif()
if(lp_solve_FOUND)
  target_link_libraries(TSARCore lp_solve)
endif()

set(TSAR_EXEC_SOURCES main.cpp)
add_executable(tsar ${TSAR_EXEC_SOURCES})
add_dependencies(tsar TSARCore)
if(NOT PACKAGE_LLVM)
  add_dependencies(tsar ${LLVM_LIBS})
endif()
target_link_libraries(tsar TSARCore ${LLVM_LIBS} BCL::Core)
set_target_properties(tsar PROPERTIES FOLDER "${TSAR_FOLDER}")
install(TARGETS tsar RUNTIME DESTINATION bin)

if (TSAR_SERVER)
  set(TSAR_SHARED_SOURCES Server.cpp PrivateServerPass.cpp)
  add_library(TSARServer SHARED ${TSAR_SHARED_SOURCES})
  add_dependencies(TSARServer TSARCore)
  if(NOT PACKAGE_LLVM)
    add_dependencies(TSARServer TSARCore ${CLANG_LIBS} ${LLVM_LIBS})
  endif()
  target_link_libraries(TSARServer TSARCore ${CLANG_LIBS} ${LLVM_LIBS} BCL::Core)
  set_target_properties(TSARServer PROPERTIES
    COMPILE_DEFINITIONS BCL_EXPORTING
    FOLDER "${TSAR_SERVER_FOLDER}")
  install(TARGETS TSARServer
    LIBRARY DESTINATION shared
    RUNTIME DESTINATION shared
    ARCHIVE DESTINATION lib)
endif()
