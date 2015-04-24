# This download LLVM sources from repository.
# Pre: subversion package must be available, ${LLVM_SOURCE_DIR} and ${LLVM_REPO} must be set.
# Post: check out a working copy ${LLVM_SOURCE_DIR} from LLVM repository ${LLVM_REPO} or
#       update it if the working copy already exist.
function(sapfor_download_llvm)
    if(NOT EXISTS ${LLVM_SOURCE_DIR})
        set(LLVM_STATUS "Checking out LLVM working copy")
        message(STATUS ${LLVM_STATUS})
        execute_process(COMMAND ${Subversion_SVN_EXECUTABLE} co ${LLVM_REPO} ${LLVM_SOURCE_DIR}
                                RESULT_VARIABLE LLVM_ERROR)
    else()
        set(LLVM_STATUS "Updating LLVM working copy")
        message(STATUS ${LLVM_STATUS})
            execute_process(COMMAND ${Subversion_SVN_EXECUTABLE} up ${LLVM_SOURCE_DIR}
                                    RESULT_VARIABLE LLVM_ERROR)
    endif()

    if(LLVM_ERROR)
       message(FATAL_ERROR "${LLVM_STATUS} - error")
    else()
       message(STATUS "${LLVM_STATUS} - done")
    endif()
endfunction(sapfor_download_llvm)