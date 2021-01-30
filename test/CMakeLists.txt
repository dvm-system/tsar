set(DVM_PATH "" CACH PATH "Path to DVM System")
if (EXISTS "${DVM_PATH}")
  get_filename_component(DVM_SCRIPT_PATH "${DVM_PATH}/user/dvm" ABSOLUTE)
endif()

if (NOT EXISTS "${DVM_SCRIPT_PATH}")
  message(WARNING "Could NOT find DVM executable which is required to run tests. "
                  "Set DVM_PATH option to run DVMH-based tests.")
endif()

add_subdirectory(perf)
add_subdirectory(analysis)
add_subdirectory(transform)
