configure_file("tsar_config.h.in" "tsar_config.h")
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

# Build executable targets
add_executable(tsar main.cpp tsar_pass.cpp tsar_private.cpp tsar_dbg_output.cpp)
add_dependencies(tsar ${LLVM_LIBS})
target_link_libraries(tsar ${LLVM_LIBS})

install(TARGETS tsar DESTINATION bin)