# Install LLVM executables such as clang, opt, llc and symlinks.
# This macro also generates convinient targets *_BUILD to build this executables
# according to options BUILD_*.
macro(sapfor_install_llvm)
  if(BUILD_CLANG)
    if(NOT CLANG_LINKS_TO_INSTALL)
      set(CLANG_LINKS_TO_INSTALL
        "$<TARGET_FILE_DIR:clang>/clang++${CMAKE_EXECUTABLE_SUFFIX}"
        "$<TARGET_FILE_DIR:clang>/clang-cl${CMAKE_EXECUTABLE_SUFFIX}"
        "$<TARGET_FILE_DIR:clang>/clang-cpp${CMAKE_EXECUTABLE_SUFFIX}")
    endif()
    add_custom_target(CLANG_BUILD ALL)
    add_dependencies(CLANG_BUILD clang)
    install(PROGRAMS $<TARGET_FILE:clang> ${CLANG_LINKS_TO_INSTALL}
      DESTINATION bin)
    install(DIRECTORY ${LLVM_BINARY_DIR}/$<CONFIG>/lib/clang DESTINATION lib
      FILES_MATCHING PATTERN *.h)
  endif()
  if(BUILD_FLANG)
    add_custom_target(FLANG_BUILD ALL)
    add_dependencies(FLANG_BUILD flang)
    install(PROGRAMS $<TARGET_FILE:flang> DESTINATION bin)
  endif()
  if(BUILD_PROFILE)
    add_custom_target(PROFILE_BUILD ALL)
    add_dependencies(PROFILE_BUILD llvm-cov llvm-profdata profile)
    install(PROGRAMS $<TARGET_FILE:llvm-cov> DESTINATION bin)
    install(PROGRAMS $<TARGET_FILE:llvm-profdata> DESTINATION bin)
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
    add_custom_target(OPT_BUILD ALL)
    add_dependencies(OPT_BUILD opt)
    install(PROGRAMS $<TARGET_FILE:opt> DESTINATION bin)
  endif()
  if(BUILD_LLC)
    add_custom_target(LLC_BUILD ALL)
    add_dependencies(LLC_BUILD llc)
    install(PROGRAMS $<TARGET_FILE:llc> DESTINATION bin)
  endif()
  if(BUILD_LINK)
    add_custom_target(LINK_BUILD ALL)
    add_dependencies(LINK_BUILD llvm-link)
    install(PROGRAMS $<TARGET_FILE:llvm-link> DESTINATION bin)
  endif()
endmacro(sapfor_install_llvm)
