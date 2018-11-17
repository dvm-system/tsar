cmake_minimum_required(VERSION 3.4.3)
project(TSAR VERSION 0.1 LANGUAGES CXX)

set(TSAR_DESCRIPTION "Traits Static Analyzer")
set(TSAR_HOMEPAGE_URL "https://bitbucket.org/dvm-system/tsar")

set (TSAR_FOLDER "" CACHE STRING "Specify IDE folder for TSAR.")
if (TSAR_FOLER)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()
find_package(BCL 3.0 REQUIRED CONFIG)
find_package(APC CONFIG QUIET)
if(NOT APC_FOUND)
  message(WARNING "Configuration without APC: APC package not found.")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake ${BCL_CMAKE_MODULE_PATH})

# Minimum LLVM version which is necessary to build TSAR.
set(LLVM_MIN_VERSION 7.0)

include(BCLCompilerOptions)
include(BCLUtils)
include(llvm-utility)

# There are four steps to configure LLVM and Clang:
# 1. Check out a working copies ${LLVM_SOURCE_DIR}, ${CLANG_SOURCE_DIR}
#    and ${COMPILER_RT_SOURCE_DIR} (or update if it already exists)
#    from LLVM repository ${LLVM_REPO}.
# 2. Apply patch in ${CMAKE_SOURCE_DIR}/patches/ to the working copies
#    ${LLVM_SOURCE_DIR} and ${CLANG_SOURCE_DIR}.
#    The patches for LLVN have names
#      llvm.patch or
#      llvm-${LLVM_VERSION_MAJOR}.patch (example llvm-3.patch) or
#      llvm-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.patch (example
#        llvm-3.4.patch) or
#      llvm-${LLVM_VERSION}.patch (example llvm-3.4.2.patch)
#    The similar names of patches are used for Clang (use 'clang-' suffix
#    instead 'llvm-').
#    All suitable patches will be applied simultaneously.
# 3. Configure LLVM project.
# 4. Specify LLVM dependences (include directories, libraries, macros etc.

option(DOWNLOAD_LLVM "Download LLVM from subversion repository" OFF)
option(LOCAL_LLVM "Use existing LLVM installation" OFF)
option(PACKAGE_LLVM "Use previously installed LLVM package" ON)

set(LLVM_REPO
  "http://llvm.org/svn/llvm-project/llvm/tags/RELEASE_700/final/" CACHE STRING
  "URL of LLVM subversion repository (usefull only if DOWNLOAD_LLVM is set)")
set(CLANG_REPO
  "http://llvm.org/svn/llvm-project/cfe/tags/RELEASE_700/final/" CACHE STRING
  "URL of Clang subversion repository (usefull only if DOWNLOAD_LLVM is set)")
set(COMPILER_RT_REPO
  "http://llvm.org/svn/llvm-project/compiler-rt/tags/RELEASE_700/final/" CACHE STRING
  "URL of Compiler-RT subversion repository (usefull only if DOWNLOAD_LLVM is set)")

set(LLVM_SOURCE_DIR "${CMAKE_BINARY_DIR}/llvm" CACHE PATH
   "Root of LLVM source tree")

set(LLVM_VERSION ${LLVM_MIN_VERSION} CACHE STRING
  "Version of installed LLVM package (usefull only if PACKAGE_LLVM is set)")

option(BUILD_TSAR "Build Traits Static Analyzer" ON)
option(BUILD_CLANG "Build LLVM native C/C++/Objective-C compiler Clang" OFF)
option(BUILD_PROFILE "Build profile runtime" OFF)
option(BUILD_OPT "Build LLVM optimizer" OFF)
option(BUILD_LLC "Build LLVM IR static compiler" OFF)
option(TSAR_SHARED "Build shared library for TSAR" OFF)

set(CLANG_SOURCE_DIR "${LLVM_SOURCE_DIR}/tools/clang")
set(COMPILER_RT_SOURCE_DIR "${LLVM_SOURCE_DIR}/projects/compiler-rt")
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
  set(TSAR_PATCH_PATCH_OPTIONS "-p2")
  set(TSAR_PATCH_SUBVERSION_OPTIONS "")
  set(TSAR_PATCH_IGNORE "IGNORE")
  set(TSAR_PATCH_COMMAND "PATCH")
else()
  if(NOT EXISTS ${LLVM_SOURCE_DIR})
    message(FATAL_ERROR "LLVM_SOURCE_DIR '${LLVM_SOURCE_DIR}' does not exist")
  endif()
  if ((BUILD_CLANG OR BUILD_TSAR) AND NOT EXISTS ${CLANG_SOURCE_DIR})
    message(FATAL_ERROR "CLANG_SOURCE_DIR '${CLANG_SOURCE_DIR}' does not exist")
  endif()
  set(LLVM_STATUS "Configuring LLVM project")
  message(STATUS ${LLVM_STATUS})
  add_subdirectory(${LLVM_SOURCE_DIR} ${LLVM_BINARY_DIR} EXCLUDE_FROM_ALL)
  find_package(LLVM CONFIG HINTS ${LLVM_BINARY_DIR} NO_DEFAULT_PATH)
  if(NOT LLVM_FOUND)
   message(FATAL_ERROR "${LLVM_STATUS} - error")
  else()
   message(STATUS "${LLVM_STATUS} - done")
  endif()
  set(LLVM_VERSION ${LLVM_VERSION} CACHE "" "" FORCE)
  set(TSAR_PATCH_PATCH_OPTIONS -p0)
  set(TSAR_PATCH_SUBVERSION_OPTIONS "")
  set(TSAR_PATCH_IGNORE IGNORE)
  set(TSAR_PATCH_COMMAND "")
  if(BUILD_PROFILE AND (NOT LLVM_TOOL_COMPILER_RT_BUILD))
    message(FATAL_ERROR
      "To build profile runtime LLVM_TOOL_COMPILER_RT_BUILD must be set to ON.")
  endif()
  sapfor_install_llvm()
endif(PACKAGE_LLVM)

include(BCLPatch)

if(BUILD_TSAR)
  if("${LLVM_VERSION}" VERSION_LESS "${LLVM_MIN_VERSION}")
  message(FATAL_ERROR
    "To build TSAR at least LLVM ${LLVM_MIN_VERSION} is required.")
endif()
  set(TSAR_PATCH_REVERSE "")
else()
  set(TSAR_PATCH_REVERSE "REVERSE")
endif()

set(TSAR_LLVM_PATCHES
  "${PROJECT_SOURCE_DIR}/patches/llvm.patch"
  "${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_VERSION_MAJOR}.patch"
  "${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.patch"
  "${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_VERSION}.patch")
set(TSAR_CLANG_PATCHES
  "${PROJECT_SOURCE_DIR}/patches/clang.patch"
  "${PROJECT_SOURCE_DIR}/patches/clang-${LLVM_VERSION_MAJOR}.patch"
  "${PROJECT_SOURCE_DIR}/patches/clang-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.patch"
  "${PROJECT_SOURCE_DIR}/patches/clang-${LLVM_VERSION}.patch")
foreach(PATCH ${TSAR_LLVM_PATCHES})
  bcl_patch(PATCHFILE ${PATCH} TARGET ${LLVM_SOURCE_DIR}
    ${TSAR_PATCH_REVERSE} ${TSAR_PATCH_IGNORE} ${TSAR_PATCH_COMMAND}
    PATCH_OPTIONS ${TSAR_PATCH_PATCH_OPTIONS}
    SUBVERSION_OPTIONS ${TSAR_PATCH_SUBVERSION_OPTIONS})
endforeach()
foreach(PATCH ${TSAR_CLANG_PATCHES})
  bcl_patch(PATCHFILE ${PATCH} TARGET ${CLANG_SOURCE_DIR}
    ${TSAR_PATCH_REVERSE} ${TSAR_PATCH_IGNORE} ${TSAR_PATCH_COMMAND}
    PATCH_OPTIONS ${TSAR_PATCH_PATCH_OPTIONS}
    SUBVERSION_OPTIONS ${TSAR_PATCH_SUBVERSION_OPTIONS})
endforeach()

if(NOT BUILD_TSAR)
  return()
endif()

set(LLVM_STATUS
  "Specifying LLVM dependences (include directories, libraries, macros etc.)")
message(STATUS ${LLVM_STATUS})

set(LLVM_COMPONENTS
  analysis asmparser bitreader core instrumentation irreader scalaropts support
  tablegen target transformutils coverage mcparser option debuginfodwarf
  x86asmparser x86info x86codegen
  powerpcasmparser powerpcinfo powerpccodegen
  aarch64asmparser aarch64info aarch64codegen
  amdgpuasmparser amdgpuinfo amdgpucodegen
  armasmparser arminfo armcodegen
  mipsasmparser mipsinfo mipscodegen
  sparcasmparser sparcinfo sparccodegen
  systemzasmparser systemzinfo systemzcodegen
  hexagoninfo hexagoncodegen
  xcoreinfo xcorecodegen
  bpfasmparser bpfinfo bpfcodegen
  nvptxinfo nvptxcodegen
  msp430info msp430codegen
  lanaiasmparser lanaiinfo lanaicodegen)
  # It seems that RISC-V architecture support in LLVM is experimental and it
  # is not a part of default build. However, LLVM build can be configured to
  # support RISC-V if necessary.
  #riscvdesc riscvinfo riscvcodegen)

llvm_map_components_to_libnames(LLVM_LIBS ${LLVM_COMPONENTS})

set(CLANG_LIBS
  clangTooling clangCodeGen clangFrontend clangDriver clangSerialization
  clangParse clangSema clangAnalysis clangRewrite clangEdit clangAST clangLex
  clangBasic clangASTMatchers clangFormat clangToolingCore)

if(NOT PACKAGE_LLVM)
  list(APPEND LLVM_INCLUDE_DIRS
    ${CLANG_SOURCE_DIR}/include ${CLANG_BINARY_DIR}/include)
endif()
include_directories(${LLVM_INCLUDE_DIRS})

if(PACKAGE_LLVM)
  list(APPEND CMAKE_MODULE_PATH ${LLVM_BINARY_DIR}/lib/cmake/llvm)
  include(AddLLVM.cmake)
  include(TableGen.cmake)
  link_directories(${LLVM_LIBRARY_DIRS})
endif()

add_definitions(${LLVM_DEFINITIONS})
message(STATUS "${LLVM_STATUS} - done")

# Set additional include directories and definitions
if(MSVC)
  file(GLOB_RECURSE TSAR_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch)
endif()

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

add_subdirectory(utils/TableGen)
add_subdirectory(src tsar)
add_subdirectory(test)
