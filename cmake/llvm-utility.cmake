macro(target_output_name target var)
 get_property(${var} TARGET ${target} PROPERTY OUTPUT_NAME)
 if (NOT ${var})
    set(${var} ${target})
  endif()
endmacro()

macro(add_external_tool target tools)
  add_custom_target(${target} ALL)
  foreach(tool ${tools})
    add_dependencies(${target} ${tool})
    foreach(C ${CMAKE_CONFIGURATION_TYPES})
      string(TOUPPER ${C} SUFFIX)
      set_target_properties(${tool} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${SUFFIX} "${CMAKE_CURRENT_BINARY_DIR}/${C}/bin")
    endforeach()
    set_target_properties(${tool} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
    install(PROGRAMS $<TARGET_FILE:${tool}> DESTINATION bin)
    target_output_name(${tool} LLVM_LINK_OUTPUT_NAME)
    set_property(GLOBAL APPEND PROPERTY TSAR_LLVM_TOOLS ${LLVM_LINK_OUTPUT_NAME})
  endforeach()
endmacro()

# Install LLVM executables such as clang, opt, llc and symlinks.
# This macro also generates convinient targets *_BUILD to build this executables
# according to options BUILD_*.
macro(sapfor_install_llvm)
  if(BUILD_CLANG)
    add_external_tool(CLANG_BUILD clang)
    if(NOT CLANG_LINKS_TO_CREATE)
      set(CLANG_LINKS_TO_CREATE clang++ clang-cl clang-cpp)
    endif()
    if (NOT CLANG_LINKS_TO_INSTALL)
      list(APPEND CMAKE_MODULE_PATH ${LLVM_SOURCE_DIR}/cmake/modules)
      foreach(L ${CLANG_LINKS_TO_CREATE})
        add_llvm_tool_symlink(${L} clang)
        list(APPEND CLANG_LINKS_TO_INSTALL "$<TARGET_FILE_DIR:clang>/${L}${CMAKE_EXECUTABLE_SUFFIX}")
      endforeach()
      set_property(GLOBAL APPEND PROPERTY TSAR_LLVM_TOOLS ${CLANG_LINKS_TO_CREATE})
    endif()
  endif()

  if(BUILD_FLANG)
    add_external_tool(FLANG_BUILD flang-new)
  endif()

  if(BUILD_PROFILE)
    add_external_tool(PROFILE_BUILD "llvm-cov;llvm-profdata")
    add_dependencies(PROFILE_BUILD profile)
    if (COMPILER_RT_DEFAULT_TARGET_TRIPLE)
      string(FIND ${COMPILER_RT_DEFAULT_TARGET_TRIPLE} "-" ARCH_DELIMITER)
    endif()
    if (ARCH_DELIMITER)
      string(SUBSTRING
        ${COMPILER_RT_DEFAULT_TARGET_TRIPLE} 0 ${ARCH_DELIMITER} DEFAULT_ARCH)
    endif()
    if (DEFAULT_ARCH)
      message(STATUS "Default profile runtime: clang_rt.profile-${DEFAULT_ARCH}")
      if (MSVC_IDE)
        # TODO (kaniandr@gmail.com): there is an error in cmake_install.cmake
        # in case of MSVC. CMake does not substitute variable $(Configuration)
        # and it is presented in cmake_install.cmake. So, we manually generate
        # path to a build library.
        set(PROFILE_LIB_PATH lib/clang/${LLVM_VERSION}/lib/windows/)
        install(PROGRAMS
          ${LLVM_BINARY_DIR}/$<CONFIG>/${PROFILE_LIB_PATH}/$<TARGET_FILE_NAME:clang_rt.profile-${DEFAULT_ARCH}>
          DESTINATION ${PROFILE_LIB_PATH})
      else()
        install(PROGRAMS $<TARGET_FILE:clang_rt.profile-${DEFAULT_ARCH}> DESTINATION lib)
      endif()
    else()
      message(WARNING "Can not determine default profile runtime")
    endif()
  endif()

  if(BUILD_OPT)
    add_external_tool(OPT_BUILD opt)
  endif()

  if(BUILD_LLC)
    add_external_tool(LLC_BUILD llc)
  endif()

  if(BUILD_LINK)
    add_external_tool(LINK_BUILD llvm-link)
  endif()
endmacro(sapfor_install_llvm)
