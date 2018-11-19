# This downloads LLVM sources from repository.
# Pre: subversion package must be available, ${LLVM_SOURCE_DIR} and ${LLVM_REPO}
#      must be set. If DOWNLOAD_CLANG is set then ${CLANG_SOURCE_DIR}
#      and ${CLANG_REPOS} must be also set.
# Post: check out a working copy ${LLVM_SOURCE_DIR} from LLVM repository
#       ${LLVM_REPO} or update it if the working copy already exist.
#       If DOWNLOAD_CLANG is set check out a working copy ${CLANG_SOURCE_DIR}
#       from Clang repository ${CLANG_REPO} or update it if the working copy
#       already exist.
function(sapfor_download_llvm)
  find_package(Subversion)
  if(NOT Subversion_FOUND)
    message(FATAL_ERROR "Subversion command line client executable is not found.")
  endif()
  if(NOT EXISTS ${LLVM_SOURCE_DIR})
    set(LLVM_STATUS "Checking out LLVM working copy")
    message(STATUS ${LLVM_STATUS})
    execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
      co ${LLVM_REPO} ${LLVM_SOURCE_DIR} RESULT_VARIABLE LLVM_ERROR)
  else()
    set(LLVM_STATUS "Updating LLVM working copy")
    message(STATUS ${LLVM_STATUS})
    execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
      up ${LLVM_SOURCE_DIR} RESULT_VARIABLE LLVM_ERROR)
  endif()
  if(DOWNLOAD_CLANG AND NOT LLVM_ERROR)
    if(NOT EXISTS ${CLANG_SOURCE_DIR})
      message(STATUS "${LLVM_STATUS} - done")
      set(LLVM_STATUS "Checking out Clang working copy")
      message(STATUS ${LLVM_STATUS})
      execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
        co ${CLANG_REPO} ${CLANG_SOURCE_DIR} RESULT_VARIABLE LLVM_ERROR)
    else()
      message(STATUS "${LLVM_STATUS} - done")
      set(LLVM_STATUS "Updating Clang working copy")
      message(STATUS ${LLVM_STATUS})
      execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
        up ${CLANG_SOURCE_DIR} RESULT_VARIABLE LLVM_ERROR)
    endif()
  endif()
  if(DOWNLOAD_COMPILER_RT AND NOT LLVM_ERROR)
    if(NOT EXISTS ${COMPILER_RT_SOURCE_DIR})
      message(STATUS "${LLVM_STATUS} - done")
      set(LLVM_STATUS "Checking out Compiler-RT working copy")
      message(STATUS ${LLVM_STATUS})
      execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
        co ${COMPILER_RT_REPO} ${COMPILER_RT_SOURCE_DIR} RESULT_VARIABLE LLVM_ERROR)
    else()
      message(STATUS "${LLVM_STATUS} - done")
      set(LLVM_STATUS "Updating Compiler-RT working copy")
      message(STATUS ${LLVM_STATUS})
      execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
        up ${COMPILER_RT_SOURCE_DIR} RESULT_VARIABLE LLVM_ERROR)
    endif()
  endif()
  if(LLVM_ERROR)
   message(FATAL_ERROR "${LLVM_STATUS} - error")
  else()
   message(STATUS "${LLVM_STATUS} - done")
  endif()
endfunction(sapfor_download_llvm)

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
  endif()
  if(BUILD_PROFILE)
    add_custom_target(PROFILE_BUILD ALL)
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
      install(PROGRAMS $<TARGET_FILE:clang_rt.profile-${DEFAULT_ARCH}> DESTINATION lib)
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
endmacro(sapfor_install_llvm)
