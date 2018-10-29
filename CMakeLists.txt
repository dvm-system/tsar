cmake_minimum_required(VERSION 2.8.0)
project(TSAR CXX)
set(TSAR_VERSION_MAJOR 0)
set(TSAR_VERSION_MINOR 1)
set(TSAR_FULL_NAME "Traits Static Analyzer")
set(TSAR_VERSION "${TSAR_VERSION_MAJOR}.${TSAR_VERSION_MINOR}")
set(TSAR_NAME "TSAR")
set(TSAR_STRING "${TSAR_NAME} ${TSAR_VERSION}")
set(TSAR_AUTHOR "Nikita A. Kataev (kataev_nik@mail.ru)")
set(TSAR_URL "http://dvm-system.org/")
set(SAPFOR_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../../..")
set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CheckCXXCompilerFlag)
include(llvm-utility)
include(utility)
include(tsar-tablegen)
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
# There are four steps to configure LLVM and Clang:
# 1. Check out a working copies ${LLVM_SOURCE_DIR} and ${CLANG_SOURCE_DIR}
#    (or update if it already exists) from LLVM repository ${LLVM_REPO}.
# 2. Apply patch in ${CMAKE_SOURCE_DIR}/patches/ to the working copies 
#    ${LLVM_SOURCE_DIR} and ${CLANG_SOURCE_DIR}.
#    The patches for LLVN have names
#      llvm.patch or 
#      llvm-${LLVM_VERSION_MAJOR}.patch (example llvm-3.patch) or
#      llvm-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.patch (example llvm-3.4.patch) or
#      llvm-${LLVM_PACKAGE_VERSION}.patch (example llvm-3.4.2.patch)
#    The similar names of patches are used for Clang (use 'clang-' suffix instead 'llvm-'). 
#    All suitable patches will be applied simultaneously
# 3. Configure LLVM project.
# 4. Specify LLVM dependences (include directories, libraries, macros etc.
set(LLVM_REPO "http://llvm.org/svn/llvm-project/llvm/tags/RELEASE_400/final/"
    CACHE STRING "URL of LLVM subversion repository (usefull only if DOWNLOAD_LLVM is set)")
set(CLANG_REPO "http://llvm.org/svn/llvm-project/cfe/tags/RELEASE_400/final/"
    CACHE STRING "URL of Clang subversion repository (usefull only if DOWNLOAD_LLVM is set)")
set(LLVM_SOURCE_DIR "${CMAKE_BINARY_DIR}/llvm" CACHE PATH "Root of LLVM source tree")
set(CLANG_SOURCE_DIR "${LLVM_SOURCE_DIR}/tools/clang")
set(LLVM_BINARY_DIR "${CMAKE_BINARY_DIR}/llvm-build")
set(CLANG_BINARY_DIR "${LLVM_BINARY_DIR}/tools/clang")
option(DOWNLOAD_LLVM "Download LLVM from subversion repository" OFF)
option(LOCAL_LLVM "Use existing LLVM installation" OFF)
option(PACKAGE_LLVM "Use previously installed LLVM package" ON)
option(BUILD_TSAR "Build Traits Static Analyzer" ON)
option(BUILD_CLANG "Build LLVM native C/C++/Objective-C compiler Clang" OFF)
option(BUILD_OPT "Build LLVM optimizer" OFF)
option(BUILD_LLC "Build LLVM IR static compiler" OFF)
option(TSAR_SHARED "Build shared library for TSAR" OFF)
set(PACKAGE_LLVM_VERSION "4.0"
 CACHE STRING "Version of installed LLVM package (usefull only if PACKAGE_LLVM is set)")
if(TSAR_SHARED AND NOT BUILD_TSAR)
  message(FATAL_ERROR "For option TSAR_SHARED the following options must be set: BUILD_tSAR")
endif()
if((DOWNLOAD_LLVM AND LOCAL_LLVM) OR
   (DOWNLOAD_LLVM AND PACKAGE_LLVM) OR
   (LOCAL_LLVM AND PACKAGE_LLVM))
  message(FATAL_ERROR "Options DOWNLOAD_LLVM, LOCAL_LLVM and PACKAGE_LLVM are not compatible")
endif()
# Download LLVM if need
set(DOWNLOAD_CLANG ${DOWNLOAD_LLVM})
if(DOWNLOAD_LLVM)
  sapfor_download_llvm()
endif()
if(LOCAL_LLVM OR DOWNLOAD_LLVM OR PACKAGE_LLVM)
  if(PACKAGE_LLVM)
    if (BUILD_CLANG OR BUILD_OPT OR BUILD_LLC)
      message(FATAL_ERROR "Options BUILD_CLANG OR BUILD_OPT OR BUILD_LLC are not compatible with PACKAGE_LLVM")
    endif()
    unset(LLVM_DIR CACHE)
    find_package(LLVM ${PACKAGE_LLVM_VERSION} REQUIRED CONFIG)
    set(LLVM_SOURCE_DIR "${LLVM_BINARY_DIR}/include/llvm")
    set(CLANG_SOURCE_DIR "${LLVM_BINARY_DIR}/include/clang")
    set(TSAR_PATCH_PATCH_OPTIONS "-p2")
    set(TSAR_PATCH_SUBVERSION_OPTIONS "")
    set(TSAR_PATCH_IGNORE "IGNORE")
    set(TSAR_PATCH_COMMAND "PATCH")
  else()
    # Configure LLVM
    if(NOT EXISTS ${LLVM_SOURCE_DIR})
      message(FATAL_ERROR "LLVM_SOURCE_DIR '${LLVM_SOURCE_DIR}' does not exist")
    endif()
    if ((BUILD_CLANG OR BUILD_TSAR) AND NOT EXISTS ${CLANG_SOURCE_DIR})
      message(FATAL_ERROR "CLANG_SOURCE_DIR '${CLANG_SOURCE_DIR}' does not exist")
    endif()
    set(LLVM_STATUS "Configuring LLVM project")
    message(STATUS ${LLVM_STATUS})
    set(SAVE_CMAKE_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/llvm-install")
    add_subdirectory(${LLVM_SOURCE_DIR} ${LLVM_BINARY_DIR} EXCLUDE_FROM_ALL)
    set(CMAKE_INSTALL_PREFIX ${SAVE_CMAKE_INSTALL_PREFIX})
    find_package(LLVM CONFIG PATHS
      "${LLVM_BINARY_DIR}/share/llvm/cmake"
      "${LLVM_BINARY_DIR}/lib/cmake/llvm"
      NO_DEFAULT_PATH)
    if(NOT LLVM_FOUND)
     message(FATAL_ERROR "${LLVM_STATUS} - error")
    else()
     message(STATUS "${LLVM_STATUS} - done")
    endif()
    set(TSAR_PATCH_PATCH_OPTIONS "-p0")
    set(TSAR_PATCH_SUBVERSION_OPTIONS "")
    set(TSAR_PATCH_IGNORE "IGNORE")
    set(TSAR_PATCH_COMMAND "")
    set(LLVM_EXECUTABLE_DIR "bin")
    if(${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5")
      if(NOT("${CMAKE_GENERATOR}" STREQUAL "Unix Makefiles"))
        set(LLVM_EXECUTABLE_DIR "${LLVM_EXECUTABLE_DIR}/\${CMAKE_INSTALL_CONFIG_NAME}")
      endif()
    else()
      if(NOT("${CMAKE_GENERATOR}" STREQUAL "Unix Makefiles"))
        set(LLVM_EXECUTABLE_DIR "\${CMAKE_INSTALL_CONFIG_NAME}/${LLVM_EXECUTABLE_DIR}")
      endif()
    endif()
    set(LLVM_EXECUTABLE_DIR "${LLVM_BINARY_DIR}/${LLVM_EXECUTABLE_DIR}")
    if(BUILD_CLANG)
      if(BUILD_TSAR AND (${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5"))
        message(FATAL_ERROR "Simultaneous build of TSAR and CLANG avaliable only if LLVM version is greater than 3.4.2 ")
      endif()
      add_custom_target(CLANG_BUILD ALL)
      add_dependencies(CLANG_BUILD clang)
      install(PROGRAMS
        "${LLVM_EXECUTABLE_DIR}/clang${CMAKE_EXECUTABLE_SUFFIX}"
        "${LLVM_EXECUTABLE_DIR}/clang++${CMAKE_EXECUTABLE_SUFFIX}"
        DESTINATION bin)
      if(NOT WIN32)
        install(PROGRAMS
          "${LLVM_EXECUTABLE_DIR}/clang-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}${CMAKE_EXECUTABLE_SUFFIX}"
          DESTINATION bin)
      endif()
    endif()
    if(BUILD_OPT)
      if(BUILD_TSAR AND (${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5"))
        message(FATAL_ERROR "Simultaneous build of TSAR and OPT avaliable only if LLVM version is greater than 3.4.2 ")
      endif()
      add_custom_target(OPT_BUILD ALL)
      add_dependencies(OPT_BUILD opt)
      install(PROGRAMS
        "${LLVM_EXECUTABLE_DIR}/opt${CMAKE_EXECUTABLE_SUFFIX}"
        DESTINATION bin)
    endif()
    if(BUILD_LLC)
      if(BUILD_TSAR AND (${LLVM_PACKAGE_VERSION} VERSION_LESS "3.5"))
        message(FATAL_ERROR "Simultaneous build of TSAR and LLC avaliable only if LLVM version is greater than 3.4.2 ")
      endif()
      add_custom_target(LLC_BUILD ALL)
      add_dependencies(LLC_BUILD llc)
      install(PROGRAMS
        "${LLVM_EXECUTABLE_DIR}/llc${CMAKE_EXECUTABLE_SUFFIX}"
        DESTINATION bin)
    endif()
  endif(PACKAGE_LLVM)
  set(TSAR_PATCH_REVERSE "")
  set(TSAR_LLVM_PATCHES
    "${PROJECT_SOURCE_DIR}/patches/llvm.patch"
    "${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_VERSION_MAJOR}.patch"
    "${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.patch"
    "${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_PACKAGE_VERSION}.patch")
  set(TSAR_CLANG_PATCHES
    "${PROJECT_SOURCE_DIR}/patches/clang.patch"
    "${PROJECT_SOURCE_DIR}/patches/clang-${LLVM_VERSION_MAJOR}.patch"
    "${PROJECT_SOURCE_DIR}/patches/clang-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.patch"
    "${PROJECT_SOURCE_DIR}/patches/clang-${LLVM_PACKAGE_VERSION}.patch")
  foreach(PATCH ${TSAR_LLVM_PATCHES})
    sapfor_patch(PATCHFILE ${PATCH} TARGET ${LLVM_SOURCE_DIR}
                 ${TSAR_PATCH_REVERSE} ${TSAR_PATCH_IGNORE} ${TSAR_PATCH_COMMAND}
                 PATCH_OPTIONS ${TSAR_PATCH_PATCH_OPTIONS}
                 SUBVERSION_OPTIONS ${TSAR_PATCH_SUBVERSION_OPTIONS})
  endforeach()
  foreach(PATCH ${TSAR_CLANG_PATCHES})
    sapfor_patch(PATCHFILE ${PATCH} TARGET ${CLANG_SOURCE_DIR}
                 ${TSAR_PATCH_REVERSE} ${TSAR_PATCH_IGNORE} ${TSAR_PATCH_COMMAND}
                 PATCH_OPTIONS ${TSAR_PATCH_PATCH_OPTIONS}
                 SUBVERSION_OPTIONS ${TSAR_PATCH_SUBVERSION_OPTIONS})
  endforeach()
  if(NOT BUILD_TSAR)
    set(TSAR_PATCH_REVERSE "REVERSE")
  endif()
  set(TSAR_LLVM_PATCHES
    "${PROJECT_SOURCE_DIR}/patches/tsar-llvm.patch"
    "${PROJECT_SOURCE_DIR}/patches/tsar-llvm-${LLVM_VERSION_MAJOR}.patch"
    "${PROJECT_SOURCE_DIR}/patches/tsar-llvm-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.patch"
    "${PROJECT_SOURCE_DIR}/patches/tsar-llvm-${LLVM_PACKAGE_VERSION}.patch")
  foreach(PATCH ${TSAR_LLVM_PATCHES})
    sapfor_patch(PATCHFILE ${PATCH} TARGET ${LLVM_SOURCE_DIR}
                 ${TSAR_PATCH_REVERSE} ${TSAR_PATCH_IGNORE} ${TSAR_PATCH_COMMAND}
                 PATCH_OPTIONS ${TSAR_PATCH_PATCH_OPTIONS}
                 SUBVERSION_OPTIONS ${TSAR_PATCH_SUBVERSION_OPTIONS})
  endforeach()
  if(BUILD_TSAR)
    set(LLVM_STATUS "Specifying LLVM dependences (include directories, libraries, macros etc.)")
    message(STATUS ${LLVM_STATUS})
       set(LLVM_COMPONENTS analysis asmparser bitreader core instrumentation
                        irreader scalaropts support tablegen target transformutils coverage
			mcparser option
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
			bpfinfo bpfcodegen
			nvptxinfo nvptxcodegen
			msp430info msp430codegen)
    if((${LLVM_PACKAGE_VERSION} VERSION_LESS "4.0"))
      set(LLVM_COMPONENTS ${LLVM_COMPONENTS} cppbackendinfo cppbackendcodegen)
    else()
      set(LLVM_COMPONENTS ${LLVM_COMPONENTS}
        lanaiasmparser lanaiinfo lanaicodegen
        riscvdesc riscvinfo riscvcodegen)
    endif()
    if(${LLVM_PACKAGE_VERSION} VERSION_LESS "3.7")
      message(FATAL_ERROR "Unsupported version of LLVM: ${LLVM_PACKAGE_VERSION}. Versions less then 3.7 is unsupported.")
      set(LLVM_COMPONENTS ${LLVM_COMPONENTS} debuginfo)
    else()
      set(LLVM_COMPONENTS ${LLVM_COMPONENTS} debuginfodwarf)
    endif()
    llvm_map_components_to_libnames(LLVM_LIBS ${LLVM_COMPONENTS})
    set(CLANG_LIBS clangTooling clangCodeGen clangFrontend clangDriver
      clangSerialization  clangParse clangSema clangAnalysis clangRewrite
      clangEdit clangAST clangLex clangBasic clangASTMatchers
      clangFormat clangToolingCore)
    if(NOT PACKAGE_LLVM)
      set(LLVM_INCLUDE_DIRS "${LLVM_INCLUDE_DIRS};${CLANG_SOURCE_DIR}/include;${CLANG_BINARY_DIR}/include")
    endif()
    include_directories(${LLVM_INCLUDE_DIRS})
    if(PACKAGE_LLVM)
      set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${LLVM_BINARY_DIR}/lib/cmake/llvm")
      include("${LLVM_BINARY_DIR}/lib/cmake/llvm/AddLLVM.cmake")
      include("${LLVM_BINARY_DIR}/lib/cmake/llvm/TableGen.cmake")
      link_directories(${LLVM_LIBRARY_DIRS})
    endif()
    add_definitions(${LLVM_DEFINITIONS})
    message(STATUS "${LLVM_STATUS} - done")
    # Set additional include directories and definitions
    set(IDB_SOURCE_DIR "${SAPFOR_SOURCE_DIR}/idb/trunk")
    include_directories("${IDB_SOURCE_DIR}/src/base")
    if(MSVC)
      file(GLOB_RECURSE BASE_HEADERS RELATIVE
      "${CMAKE_CURRENT_SOURCE_DIR}/src" "${IDB_SOURCE_DIR}/src/base/*.h")
      file(GLOB_RECURSE TSAR_PATCHES RELATIVE
      "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
    endif()
    if(NOT LLVM_ENABLE_RTTI)
      if(TSAR_COMPILER_IS_GCC_COMPATIBLE)
        sapfor_replace_compiler_option(CMAKE_CXX_FLAGS "-frtti" "-fno-rtti")
      elseif(MSVC)
        sapfor_replace_compiler_option(CMAKE_CXX_FLAGS "/GR" "/GR-")
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
  endif()
endif()
