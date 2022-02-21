set(APC_SOURCES Passes.cpp APCContext.cpp Utils.cpp LoopInfo.cpp
  ArrayInfo.cpp FunctionInfo.cpp Parallelization.cpp ClangDVMHWriter.cpp
  DistributionLimits.cpp DirectivesCollector.cpp DiagnosticPrinter.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE APC_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/APC/*.h)
  file(GLOB_RECURSE APC_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(apc FILES ${APC_CORE_HEADERS})
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(APC STATIC ${APC_SOURCES} ${APC_HEADERS} ${APC_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(APC ${CLANG_LIBS} ${LLVM_LIBS})
endif()
add_dependencies(APC DirectivesGen DiagnosticKinds IntrinsicsGen AttributesGen)
target_link_libraries(APC TSARTransformClang BCL::Core APC::APCCore)

add_definitions("-D__SPC")
set_target_properties(APC PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
