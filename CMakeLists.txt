cmake_minimum_required(VERSION 2.8.0)

find_package(Subversion)
if(NOT Subversion_FOUND)
    message(FATAL_ERROR "Subversion command line client executable is not found.")
endif()

project(TSAR CXX)

set(SAPFOR_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../../..")
set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

add_definitions("-D${PROJECT_NAME}_PROJECT")

# Determine type of compiler to specify flags properly.
# This code is taken from LLVM (cmake/modulesHandleLLVMOptions.cmake).
if(CMAKE_COMPILER_IS_GNUCXX)
    set(TSAR_COMPILER_IS_GCC_COMPATIBLE ON)
elseif(MSVC)
    set(TSAR_COMPILER_IS_GCC_COMPATIBLE OFF)
elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(LLVM_COMPILER_IS_GCC_COMPATIBLE ON)
endif()

# There are four steps to configure LLVM:
# 1. Check out a working copy ${LLVM_SOURCE_DIR} (or update if it already exists) from LLVM repository ${LLVM_REPO}
# 2. Apply patch in ${CMAKE_SOURCE_DIR}/patches/llvm.patch to the working copy ${LLVM_SOURCE_DIR}
# 3. Configure LLVM project
# 4. Specify LLVM dependences (include directories, libraries, macros etc.)
set(LLVM_REPO "http://llvm.org/svn/llvm-project/llvm/tags/RELEASE_342/final/"
    CACHE STRING "URL of LLVM subversion repository (usefull only if DOWNLOAD_LLVM is set)")
set(LLVM_SOURCE_DIR "${CMAKE_BINARY_DIR}/llvm" CACHE PATH "Root of LLVM source tree")
set(LLVM_BINARY_DIR "${CMAKE_BINARY_DIR}/llvm-build")
set(LLVM_DIR "${LLVM_BINARY_DIR}/share/llvm/cmake")
set(LLVM_COMPONENTS analysis asmparser bitreader core debuginfo instrumentation
                    irreader scalaropts support tablegen target transformutils)

option(DOWNLOAD_LLVM "Download LLVM from subversion repository" OFF)
option(LOCAL_LLVM "Use existing LLVM installation" OFF)

if(DOWNLOAD_LLVM AND LOCAL_LLVM)
    message(FATAL_ERROR "Options DOWNLOAD_LLVM and LOCAL_LLVM are not compatible")
endif()

include(llvm-utility)
include(utility)

# Download LLVM if need
if(DOWNLOAD_LLVM)
    sapfor_download_llvm()
elseif(LOCAL_LLVM)
    unset(LLVM_REPO CACHE)
endif()

if(LOCAL_LLVM OR DOWNLOAD_LLVM)
# Configure LLVM
    if(NOT EXISTS ${LLVM_SOURCE_DIR})
        message(FATAL_ERROR "LLVM_SOURCE_DIR '${LLVM_SOURCE_DIR}' does not exist")
    endif()

    set(LLVM_STATUS "Configuring LLVM project")
    message(STATUS ${LLVM_STATUS})
    set(SAVE_CMAKE_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/llvm-install")
    add_subdirectory(${LLVM_SOURCE_DIR} ${LLVM_BINARY_DIR} EXCLUDE_FROM_ALL)
    set(CMAKE_INSTALL_PREFIX ${SAVE_CMAKE_INSTALL_PREFIX})
    find_package(LLVM CONFIG)
    if(NOT LLVM_FOUND)
       message(FATAL_ERROR "${LLVM_STATUS} - error")
    else()
       message(STATUS "${LLVM_STATUS} - done")
    endif()

    set(LLVM_STATUS "Applying patch to the LLVM working copy")
    message(STATUS ${LLVM_STATUS})
    execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
                            patch "${PROJECT_SOURCE_DIR}/patches/llvm.patch" ${LLVM_SOURCE_DIR}
                            RESULT_VARIABLE LLVM_ERROR)
    if(LLVM_ERROR)
       message(FATAL_ERROR "${LLVM_STATUS} - error")
    else()
       message(STATUS "${LLVM_STATUS} - done")
    endif()

    if (EXISTS "${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_PACKAGE_VERSION}.patch")
        set(LLVM_STATUS "Applying version dependent patch (${LLVM_PACKAGE_VERSION}) to the LLVM working copy")
        message(STATUS ${LLVM_STATUS})
        execute_process(COMMAND ${Subversion_SVN_EXECUTABLE}
                                patch "${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_PACKAGE_VERSION}.patch"
                                ${LLVM_SOURCE_DIR}
                                RESULT_VARIABLE LLVM_ERROR)
        if(LLVM_ERROR)
           message(FATAL_ERROR "${LLVM_STATUS} - error")
        else()
           message(STATUS "${LLVM_STATUS} - done")
        endif()
    endif()

    set(LLVM_STATUS "Specifying LLVM dependences (include directories, libraries, macros etc.)")
    message(STATUS ${LLVM_STATUS})
    if(${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5")
        llvm_map_components_to_libraries(LLVM_LIBS ${LLVM_COMPONENTS})
        set(LLVM_INCLUDE_DIRS "${LLVM_SOURCE_DIR}/include;${LLVM_BINARY_DIR}/include")
        set(LLVM_LIBRARY_DIRS "${LLVM_BINARY_DIR}/$(Configuration)/lib")
        set(LLVM_TOOLS_BINARY_DIR "${LLVM_BINARY_DIR}/$(Configuration)/bin")
    else()
        llvm_map_components_to_libnames(LLVM_LIBS ${LLVM_COMPONENTS})
    endif()
    include_directories(${LLVM_INCLUDE_DIRS})
    add_definitions(${LLVM_DEFINITIONS})
    if((${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5"))
        link_directories(${LLVM_LIBRARY_DIRS})
    endif()
    message(STATUS "${LLVM_STATUS} - done")   

# Set additional include directories and definitions
    set(IDB_SOURCE_DIR "${SAPFOR_SOURCE_DIR}/idb/trunk")
    include_directories("${IDB_SOURCE_DIR}/src/base")
    
    if(${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5" OR NOT LLVM_ENABLE_RTTI)
        if(TSAR_COMPILER_IS_GCC_COMPATIBLE)
            sapfor_replace_compiler_option(CMAKE_CXX_FLAGS "-frtti" "-fno-rtti")
        elseif(MSVC)
            sapfor_replace_compiler_option(CMAKE_CXX_FLAGS "/GR" "/GR-")
        endif()
    endif()

    add_subdirectory(src)
endif()