set(TSAR_EXEC_SOURCES main.cpp)

add_executable(tsar ${TSAR_EXEC_SOURCES})

if(MSVC_IDE)
  file(GLOB ANALYSIS_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
  if (APC_FOUND)
    source_group(apc FILES ${APC_CORE_HEADERS})
  endif()
  if(lp_solve_FOUND)
    source_group(lp_solve FILES ${lp_solve_HEADERS})
  endif()
endif()

add_dependencies(tsar TSARTool)
if(NOT PACKAGE_LLVM)
  add_dependencies(tsar ${CLANG_LIBS} ${FLANG_LIBS} ${LLVM_LIBS})
endif()
target_link_libraries(tsar TSARTool ${CLANG_LIBS} ${FLANG_LIBS} ${LLVM_LIBS} BCL::Core)

set_target_properties(tsar PROPERTIES FOLDER "${TSAR_FOLDER}")

install(TARGETS tsar EXPORT TSARExports RUNTIME DESTINATION bin)
