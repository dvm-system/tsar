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
  if(NOT EXISTS ${LLVM_SOURCE_DIR})
    set(LLVM_STATUS "Checking out LLVM working copy")
    message(STATUS ${LLVM_STATUS})
    execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
      co ${LLVM_REPO} ${LLVM_SOURCE_DIR} RESULT_VARIABLE LLVM_ERROR)
    if(DOWNLOAD_CLANG AND NOT LLVM_ERROR)
      message(STATUS "${LLVM_STATUS} - done")
      set(LLVM_STATUS "Checking out Clang working copy")
      message(STATUS ${LLVM_STATUS})
      execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
        co ${CLANG_REPO} ${CLANG_SOURCE_DIR} RESULT_VARIABLE LLVM_ERROR)
    endif()
  else()
    set(LLVM_STATUS "Updating LLVM working copy")
    message(STATUS ${LLVM_STATUS})
    execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
      up ${LLVM_SOURCE_DIR} RESULT_VARIABLE LLVM_ERROR)
    if(DOWNLOAD_CLANG AND NOT LLVM_ERROR)
      message(STATUS "${LLVM_STATUS} - done")
      set(LLVM_STATUS "Updating Clang working copy")
      message(STATUS ${LLVM_STATUS})
      execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
        up ${CLANG_SOURCE_DIR} RESULT_VARIABLE LLVM_ERROR)
    endif()
  endif()    
  if(LLVM_ERROR)
   message(FATAL_ERROR "${LLVM_STATUS} - error")
  else()
   message(STATUS "${LLVM_STATUS} - done")
  endif()
endfunction(sapfor_download_llvm)