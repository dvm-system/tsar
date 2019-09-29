cmake_minimum_required(VERSION 3.4.3)
project(TSAR VERSION 0.1 LANGUAGES CXX)

set(TSAR_DESCRIPTION "Traits Static Analyzer")
set(TSAR_HOMEPAGE_URL "https://bitbucket.org/dvm-system/tsar")

set (TSAR_FOLDER "Tools" CACHE STRING "Specify IDE folder for TSAR executable.")
set (TSAR_LIBRARY_FOLDER "Tsar libraries" CACHE STRING "Specify IDE folder for TSAR libraries.")
set (TSAR_SERVER_FOLDER "Tools" CACHE STRING "Specify IDE folder for TSAR server library.")
if (TSAR_FOLDER OR TSAR_LIBRARY_FOLDER)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()

find_package(BCL 3.0 REQUIRED CONFIG)

option(BUILD_APC "Build automated parallelizing compiler" OFF)
if (BUILD_APC)
  find_package(APC CONFIG REQUIRED)
  message(STATUS "APC version: ${APC_VERSION}")
endif()

option(BUILD_lp_solve "Build Mixed Integer Linear Programming (MILP) solver" OFF)
if (BUILD_lp_solve)
  find_package(lp_solve CONFIG QUIET)
  message(STATUS "lp_solve version: ${lp_solve_VERSION}")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake ${BCL_CMAKE_MODULE_PATH})

# Minimum LLVM version which is necessary to build TSAR.
set(LLVM_MIN_VERSION 7.0)

include(BCLCompilerOptions)
include(BCLUtils)
include(llvm-utility)

# There are three steps to configure LLVM and Clang:
# 1.a (DOWNLOAD_LLVM is ON, SVN is currently supported only):
#    check out a working copies ${LLVM_SOURCE_DIR}, ${CLANG_SOURCE_DIR}
#    and ${COMPILER_RT_SOURCE_DIR} (or update if it already exists)
#    from LLVM repository ${LLVM_REPO}.
# 1.b (LOCAL_LLVM is ON, SVN multirepo or GIT multi/mono repo are supported):
#    Use LLVM as TSAR subproject (add_subdirectory() is used to build LLVM).
# 1.c (PACKAGE_LLVM is ON):
#    Use previously installed LLVM package.
# 2. Configure LLVM project ((in case of 1.a and 1.b only).
# 4. Specify LLVM dependences (include directories, libraries, macros etc.

option(DOWNLOAD_LLVM "Download LLVM from subversion repository" OFF)
option(LOCAL_LLVM "Use existing LLVM sources" OFF)
option(PACKAGE_LLVM "Use previously installed LLVM package" ON)

set(LLVM_REPO
  "http://llvm.org/svn/llvm-project/llvm/tags/RELEASE_700/final/" CACHE STRING
  "URL of LLVM subversion repository (useful only if DOWNLOAD_LLVM is set)")
set(CLANG_REPO
  "http://llvm.org/svn/llvm-project/cfe/tags/RELEASE_700/final/" CACHE STRING
  "URL of Clang subversion repository (useful only if DOWNLOAD_LLVM is set)")
set(COMPILER_RT_REPO
  "http://llvm.org/svn/llvm-project/compiler-rt/tags/RELEASE_700/final/" CACHE STRING
  "URL of Compiler-RT subversion repository (useful only if DOWNLOAD_LLVM is set)")

set(LLVM_SOURCE_DIR "${CMAKE_BINARY_DIR}/llvm" CACHE PATH
   "Root of LLVM source tree")

set(LLVM_VERSION ${LLVM_MIN_VERSION} CACHE STRING
  "Version of installed LLVM package (useful only if PACKAGE_LLVM is set)")

option(BUILD_TSAR "Build Traits Static Analyzer" ON)
option(BUILD_CLANG "Build LLVM native C/C++/Objective-C compiler Clang" OFF)
option(BUILD_PROFILE "Build profile runtime" OFF)
option(BUILD_OPT "Build LLVM optimizer" OFF)
option(BUILD_LLC "Build LLVM IR static compiler" OFF)
option(TSAR_SERVER "Build TSAR server shared library" OFF)

option(TSAR_ENABLE_LLVM_DUMP "Enable use of dump() method for LLVM types" ON)
option(LLVM_MONO_REPO
  "Use LLVM monolithic repository (useful only if LOCAL_LLVM is set)" ON)

if(NOT LLVM_MONO_REPO)
  set(CLANG_SOURCE_DIR "${LLVM_SOURCE_DIR}/tools/clang")
  set(COMPILER_RT_SOURCE_DIR "${LLVM_SOURCE_DIR}/projects/compiler-rt")
else()
  set(CLANG_SOURCE_DIR "${LLVM_SOURCE_DIR}/../clang")
  set(COMPILER_RT_SOURCE_DIR "${LLVM_SOURCE_DIR}/../compiler-rt")
endif()

set(LLVM_BINARY_DIR "${CMAKE_BINARY_DIR}/llvm-build")
set(CLANG_BINARY_DIR "${LLVM_BINARY_DIR}/tools/clang")

if((DOWNLOAD_LLVM AND LOCAL_LLVM) OR
   (DOWNLOAD_LLVM AND PACKAGE_LLVM) OR
   (LOCAL_LLVM AND PACKAGE_LLVM))
  message(FATAL_ERROR
    "Options DOWNLOAD_LLVM, LOCAL_LLVM and PACKAGE_LLVM are not compatible")
endif()
if(NOT LOCAL_LLVM AND NOT DOWNLOAD_LLVM AND NOT PACKAGE_LLVM)
  message(FATAL_ERROR
    "One of options LOCAL_LLVM, DOWNLOAD_LLVM, PACKAGE_LLVM should be specified.")
endif()
if (DOWNLOAD_LLVM AND LLVM_MONO_REPO)
  message(FATAL_ERROR
    "Options DOWNLOAD_LLVM and LLVM_MONO_REPO are not compatible: "
    "download monolithic repository manually "
    "or use Subversion umbrella repository")
endif()

# Download LLVM if need
set(DOWNLOAD_CLANG ${DOWNLOAD_LLVM})
if (BUILD_PROFILE)
  set(DOWNLOAD_COMPILER_RT ${DOWNLOAD_LLVM})
endif()
if(DOWNLOAD_LLVM)
  sapfor_download_llvm()
endif()

if(PACKAGE_LLVM)
  if (BUILD_CLANG OR BUILD_OPT OR BUILD_LLC OR BUILD_PROFILE)
    message(FATAL_ERROR
      "Options BUILD_CLANG OR BUILD_OPT OR BUILD_LLC OR BUILD_PROFILE are not compatible with PACKAGE_LLVM")
  endif()
  unset(LLVM_DIR CACHE)
  find_package(LLVM ${LLVM_VERSION} REQUIRED CONFIG)
  set(LLVM_SOURCE_DIR "${LLVM_BINARY_DIR}/include/llvm")
  set(CLANG_SOURCE_DIR "${LLVM_BINARY_DIR}/include/clang")
else()
  if(NOT EXISTS ${LLVM_SOURCE_DIR})
    message(FATAL_ERROR "LLVM_SOURCE_DIR '${LLVM_SOURCE_DIR}' does not exist")
  endif()
  if ((BUILD_CLANG OR BUILD_TSAR) AND NOT EXISTS ${CLANG_SOURCE_DIR})
    message(FATAL_ERROR "CLANG_SOURCE_DIR '${CLANG_SOURCE_DIR}' does not exist")
  endif()
  set(LLVM_STATUS "Configuring LLVM project")
  message(STATUS ${LLVM_STATUS})
  if (LLVM_MONO_REPO)
    set(LLVM_PROJECTS clang)
    if (BUILD_PROFILE)
      set(LLVM_PROJECTS ${LLVM_PROJECTS} compiler-rt)
    endif()
    set(LLVM_ENABLE_PROJECTS ${LLVM_PROJECTS} CACHE STRING "" FORCE)
  endif()
  add_subdirectory(${LLVM_SOURCE_DIR} ${LLVM_BINARY_DIR} EXCLUDE_FROM_ALL)
  find_package(LLVM CONFIG HINTS ${LLVM_BINARY_DIR} NO_DEFAULT_PATH)
  if(NOT LLVM_FOUND)
   message(FATAL_ERROR "${LLVM_STATUS} - error")
  else()
   message(STATUS "${LLVM_STATUS} - done")
  endif()
  set(LLVM_VERSION ${LLVM_VERSION} CACHE STRING "" FORCE)
  if(BUILD_PROFILE AND (NOT LLVM_TOOL_COMPILER_RT_BUILD))
    message(FATAL_ERROR
      "To build profile runtime LLVM_TOOL_COMPILER_RT_BUILD must be set to ON.")
  endif()
  sapfor_install_llvm()
endif(PACKAGE_LLVM)

if(BUILD_TSAR)
  if("${LLVM_VERSION}" VERSION_LESS "${LLVM_MIN_VERSION}")
    message(FATAL_ERROR
      "To build TSAR at least LLVM ${LLVM_MIN_VERSION} is required.")
  endif()
else()
  return()
endif()

set(LLVM_STATUS
  "Specifying LLVM dependences (include directories, libraries, macros etc.)")
message(STATUS ${LLVM_STATUS})

set(LLVM_COMPONENTS
  analysis asmparser bitreader core instrumentation irreader scalaropts support
  tablegen target transformutils coverage mcparser option debuginfodwarf)

llvm_map_components_to_libnames(LLVM_LIBS ${LLVM_COMPONENTS})

# Add targets to the list of necessary LLVM libraries (LLVM_LIBS).
foreach(T ${LLVM_TARGETS_TO_BUILD})
  llvm_map_components_to_libnames(L "${T}asmparser")
  list(FIND LLVM_AVAILABLE_LIBS "${L}" FIND_RES)
  if (NOT ${FIND_RES} EQUAL -1)
    list(APPEND LLVM_LIBS ${L})
  endif()
  llvm_map_components_to_libnames(L "${T}info")
  list(FIND LLVM_AVAILABLE_LIBS ${L} FIND_RES)
  if (NOT ${FIND_RES} EQUAL -1)
    list(APPEND LLVM_LIBS "${L}")
  endif()
  llvm_map_components_to_libnames(L "${T}codegen")
  list(FIND LLVM_AVAILABLE_LIBS "${L}" FIND_RES)
  if (NOT ${FIND_RES} EQUAL -1)
    list(APPEND LLVM_LIBS ${L})
  endif()
endforeach()

set(CLANG_LIBS
  clangTooling clangCodeGen clangFrontend clangDriver clangSerialization
  clangParse clangSema clangAnalysis clangRewrite clangEdit clangAST clangLex
  clangBasic clangASTMatchers clangFormat clangToolingCore clangToolingInclusions)

if(NOT PACKAGE_LLVM)
  list(APPEND LLVM_INCLUDE_DIRS
    ${CLANG_SOURCE_DIR}/include ${CLANG_BINARY_DIR}/include)
endif()
include_directories(${LLVM_INCLUDE_DIRS})

if(PACKAGE_LLVM)
  list(APPEND CMAKE_MODULE_PATH ${LLVM_BINARY_DIR}/lib/cmake/llvm)
  include(AddLLVM)
  include(TableGen)
  link_directories(${LLVM_LIBRARY_DIRS})
endif()

add_definitions(${LLVM_DEFINITIONS})
message(STATUS "${LLVM_STATUS} - done")

# Set additional include directories and definitions
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR})

if(NOT LLVM_ENABLE_RTTI)
  if(BCL_COMPILER_IS_GCC_COMPATIBLE)
    bcl_replace(CMAKE_CXX_FLAGS "-frtti" "-fno-rtti")
  elseif(MSVC)
    bcl_replace(CMAKE_CXX_FLAGS "/GR" "/GR-")
  endif()
endif()

if(DEFINED LLVM_BUILD_TYPE)
  if ("${LLVM_BUILD_TYPE}" STREQUAL Debug)
    set(LLVM_DEBUG_BUILD ${LLVM_BUILD_TYPE})
  elseif (NOT("${LLVM_BUILD_TYPE}" STREQUAL ""))
    set(LLVM_RELEASE_BUILD ${LLVM_BUILD_TYPE})
  endif()
endif()

# To avoid conflicts between linked library appropriate run-time library
# must be specified manually.
if (MSVC)
  option(LLVM_PACKAGE_DEBUG
   "LLVM package has debug build type (useful only if PACKAGE_LLVM is set)" OFF)
  if (PACKAGE_LLVM)
    if (LLVM_PACKAGE_DEBUG)
      set(LLVM_DEBUG_BUILD "\"\"")
      add_compile_options("/MDd")
    else()
      set(LLVM_RELEASE_BUILD "\"\"")
      add_compile_options("/MD")
    endif()
  endif()
endif()

add_subdirectory(utils/TableGen)
add_subdirectory(lib tsar)
add_subdirectory(test)
add_subdirectory(tools)

set_target_properties(${TSAR_TABLEGEN} PROPERTIES FOLDER "Tablegenning")
