cmake_minimum_required(VERSION 2.8.0)

project(TSAR CXX)

set(TSAR_VERSION_MAJOR 0)
set(TSAR_VERSION_MINOR 1)
set(TSAR_FULL_NAME "Traits Static Analyzer")
set(TSAR_VERSION "${TSAR_VERSION_MAJOR}.${TSAR_VERSION_MINOR}")
set(TSAR_NAME "TSAR")
set(TSAR_STRING "${TSAR_NAME} ${TSAR_VERSION}")
set(TSAR_AUTHOR "Nikita A. Kataev (kataev_nik@mail.ru)")


set(SAPFOR_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../../..")
set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CheckCXXCompilerFlag)
include(llvm-utility)
include(utility)

find_package(Subversion)
if(NOT Subversion_FOUND)
    message(FATAL_ERROR "Subversion command line client executable is not found.")
endif()

# Determine type of compiler to specify flags properly.
# This code is taken from LLVM (cmake/modulesHandleLLVMOptions.cmake).
if(CMAKE_COMPILER_IS_GNUCXX)
    set(TSAR_COMPILER_IS_GCC_COMPATIBLE ON)
elseif(MSVC)
    set(TSAR_COMPILER_IS_GCC_COMPATIBLE OFF)
elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(TSAR_COMPILER_IS_GCC_COMPATIBLE ON)
endif()


# Enable C++ 11 support.
# This code is taken from LLVM (cmake/modulesHandleLLVMOptions.cmake).
if(TSAR_COMPILER_IS_GCC_COMPATIBLE)
    check_cxx_compiler_flag("-std=c++11" CXX_SUPPORTS_CXX11)
    if(CXX_SUPPORTS_CXX11)
      if(CYGWIN OR MINGW)
        # MinGW and Cygwin are a bit stricter and lack things like
        # 'strdup', 'stricmp', etc in c++11 mode.
        append("-std=gnu++11" CMAKE_CXX_FLAGS)
      else()
        append("-std=c++11" CMAKE_CXX_FLAGS)
      endif()
    else()
      message(FATAL_ERROR "TSAR requires C++11 support but the '-std=c++11' flag isn't supported.")
    endif()
endif(TSAR_COMPILER_IS_GCC_COMPATIBLE)

# There are four steps to configure LLVM:
# 1. Check out a working copy ${LLVM_SOURCE_DIR} (or update if it already exists) from LLVM repository ${LLVM_REPO}
# 2. Apply patch in ${CMAKE_SOURCE_DIR}/patches/llvm.patch to the working copy ${LLVM_SOURCE_DIR}
# 3. Configure LLVM project
# 4. Specify LLVM dependences (include directories, libraries, macros etc.)
set(LLVM_REPO "http://llvm.org/svn/llvm-project/llvm/tags/RELEASE_350/final/"
    CACHE STRING "URL of LLVM subversion repository (usefull only if DOWNLOAD_LLVM is set)")
set(CLANG_REPO "http://llvm.org/svn/llvm-project/cfe/tags/RELEASE_350/final/"
    CACHE STRING "URL of Clang subversion repository (usefull only if DOWNLOAD_LLVM and BUILD_CLANG are set)")
set(LLVM_SOURCE_DIR "${CMAKE_BINARY_DIR}/llvm" CACHE PATH "Root of LLVM source tree")
set(CLANG_SOURCE_DIR "${LLVM_SOURCE_DIR}/tools/clang")
set(LLVM_BINARY_DIR "${CMAKE_BINARY_DIR}/llvm-build")
set(LLVM_DIR "${LLVM_BINARY_DIR}/share/llvm/cmake")

option(DOWNLOAD_LLVM "Download LLVM from subversion repository" OFF)
option(LOCAL_LLVM "Use existing LLVM installation" OFF)
option(BUILD_TSAR "Build Traits Static Analyzer" ON)
option(BUILD_CLANG "Build LLVM native C/C++/Objective-C compiler Clang" OFF)
option(BUILD_OPT "Build LLVM optimizer" OFF)
option(BUILD_LLC "Build LLVM IR static compiler" OFF)

if(DOWNLOAD_LLVM AND LOCAL_LLVM)
  message(FATAL_ERROR "Options DOWNLOAD_LLVM and LOCAL_LLVM are not compatible")
endif()

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
  if (BUILD_CLANG AND NOT EXISTS ${CLANG_SOURCE_DIR})
    message(FATAL_ERROR "CLANG_SOURCE_DIR '${CLANG_SOURCE_DIR}' does not exist")
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
  if(BUILD_CLANG)
    if(BUILD_TSAR AND (${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5"))
        message(FATAL_ERROR "Simultaneous build of TSAR and CLANG avaliable only if LLVM version is greater than 3.4.2 ")
    endif()
    add_custom_target(CLANG_BUILD ALL)
    add_dependencies(CLANG_BUILD clang)
    if(${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5")
      install(PROGRAMS
        "${LLVM_BINARY_DIR}/bin/\${CMAKE_INSTALL_CONFIG_NAME}/clang${CMAKE_EXECUTABLE_SUFFIX}"
        "${LLVM_BINARY_DIR}/bin/\${CMAKE_INSTALL_CONFIG_NAME}/clang++${CMAKE_EXECUTABLE_SUFFIX}"
        DESTINATION bin)
    else()
      install(PROGRAMS
        "${LLVM_BINARY_DIR}/\${CMAKE_INSTALL_CONFIG_NAME}/bin/clang${CMAKE_EXECUTABLE_SUFFIX}"
        "${LLVM_BINARY_DIR}/\${CMAKE_INSTALL_CONFIG_NAME}/bin/clang++${CMAKE_EXECUTABLE_SUFFIX}"
        DESTINATION bin)
    endif()
  endif()
  if(BUILD_OPT)
    if(BUILD_TSAR AND (${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5"))
      message(FATAL_ERROR "Simultaneous build of TSAR and OPT avaliable only if LLVM version is greater than 3.4.2 ")
    endif()
    add_custom_target(OPT_BUILD ALL)
    add_dependencies(OPT_BUILD opt)
    if(${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5")
      install(PROGRAMS
        "${LLVM_BINARY_DIR}/bin/\${CMAKE_INSTALL_CONFIG_NAME}/opt${CMAKE_EXECUTABLE_SUFFIX}"
        DESTINATION bin)
    else()
      install(PROGRAMS
        "${LLVM_BINARY_DIR}/\${CMAKE_INSTALL_CONFIG_NAME}/bin/opt${CMAKE_EXECUTABLE_SUFFIX}"
        DESTINATION bin)
    endif()
  endif()
  if(BUILD_LLC)
    if(BUILD_TSAR AND (${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5"))
      message(FATAL_ERROR "Simultaneous build of TSAR and LLC avaliable only if LLVM version is greater than 3.4.2 ")
    endif()
    add_custom_target(LLC_BUILD ALL)
    add_dependencies(LLC_BUILD llc)
    if(${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5")
      install(PROGRAMS
        "${LLVM_BINARY_DIR}/bin/\${CMAKE_INSTALL_CONFIG_NAME}/llc${CMAKE_EXECUTABLE_SUFFIX}"
        DESTINATION bin)
    else()
      install(PROGRAMS
        "${LLVM_BINARY_DIR}/\${CMAKE_INSTALL_CONFIG_NAME}/bin/llc${CMAKE_EXECUTABLE_SUFFIX}"
        DESTINATION bin)
    endif()
  endif()
  sapfor_patch("${PROJECT_SOURCE_DIR}/patches/llvm.patch" ${LLVM_SOURCE_DIR})
  sapfor_patch("${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_VERSION_MAJOR}.patch" ${LLVM_SOURCE_DIR})
  sapfor_patch("${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.patch" ${LLVM_SOURCE_DIR})
  sapfor_patch("${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_PACKAGE_VERSION}.patch" ${LLVM_SOURCE_DIR})
  if(BUILD_CLANG)
    sapfor_patch("${PROJECT_SOURCE_DIR}/patches/clang.patch" ${CLANG_SOURCE_DIR})
    sapfor_patch("${PROJECT_SOURCE_DIR}/patches/clang-${LLVM_VERSION_MAJOR}.patch" ${CLANG_SOURCE_DIR})
    sapfor_patch("${PROJECT_SOURCE_DIR}/patches/clang-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.patch" ${CLANG_SOURCE_DIR})
    sapfor_patch("${PROJECT_SOURCE_DIR}/patches/clang-${LLVM_PACKAGE_VERSION}.patch" ${CLANG_SOURCE_DIR})
  endif()
  if(NOT BUILD_TSAR)
    set(TSAR_PATCH_REVERS "--reverse-diff")
  else()
    set(TSAR_PATCH_REVERS "")
  endif()
  sapfor_patch("${PROJECT_SOURCE_DIR}/patches/tsar-llvm.patch" ${LLVM_SOURCE_DIR} ${TSAR_PATCH_REVERS})
  sapfor_patch("${PROJECT_SOURCE_DIR}/patches/tsar-llvm-${LLVM_VERSION_MAJOR}.patch" ${LLVM_SOURCE_DIR} ${TSAR_PATCH_REVERS})
  sapfor_patch("${PROJECT_SOURCE_DIR}/patches/tsar-llvm-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.patch" ${LLVM_SOURCE_DIR} ${TSAR_PATCH_REVERS})
  sapfor_patch("${PROJECT_SOURCE_DIR}/patches/tsar-llvm-${LLVM_PACKAGE_VERSION}.patch" ${LLVM_SOURCE_DIR} ${TSAR_PATCH_REVERS})
  if(BUILD_TSAR)
    set(LLVM_STATUS "Specifying LLVM dependences (include directories, libraries, macros etc.)")
    message(STATUS ${LLVM_STATUS})
    set(LLVM_COMPONENTS analysis asmparser bitreader core debuginfo instrumentation
                        irreader scalaropts support tablegen target transformutils)
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
    add_subdirectory(src tsar)
  endif()
endif()