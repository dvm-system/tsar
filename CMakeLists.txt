cmake_minimum_required(VERSION 3.4.3)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(TSAR VERSION 0.0.0 LANGUAGES C CXX)

if(SAPFOR_VERSION)
  set(TSAR_VERSION ${SAPFOR_VERSION})
  set(TSAR_VERSION_MAJOR ${SAPFOR_VERSION_MAJOR})
  set(TSAR_VERSION_MINOR ${SAPFOR_VERSION_MINOR})
  set(TSAR_VERSION_PATCH ${SAPFOR_VERSION_PATCH})
  set(TSAR_VERSION_SUFFIX ${SAPFOR_VERSION_SUFFIX})

  set(PROJECT_VERSION ${TSAR_VERSION})
  set(PROJECT_VERSION_MAJOR ${TSAR_VERSION_MAJOR})
  set(PROJECT_VERSION_MINOR ${TSAR_VERSION_MINOR})
  set(PROJECT_VERSION_PATCH ${TSAR_VERSION_PATCH})
endif()

option(TSAR_DIRTY_BUILD "Do not use commit-based TSAR version string" ON)
set(TSAR_VERSION_BUILD "dirty")
if (NOT TSAR_DIRTY_BUILD)
  find_package(Git)
  if (GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} log -1 --pretty=format:%h
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      RESULT_VARIABLE TSAR_SHA_ERROR OUTPUT_VARIABLE TSAR_SHA)
    if (NOT TSAR_SHA_ERROR)
      set(TSAR_VERSION_BUILD ${TSAR_SHA})
    endif()
  endif()
endif()
set_property(GLOBAL PROPERTY TSAR_VERSION_BUILD ${TSAR_VERSION_BUILD})
set(TSAR_VERSION "${TSAR_VERSION}+${TSAR_VERSION_BUILD}")

set(TSAR_DESCRIPTION "Traits Static Analyzer")
set(TSAR_HOMEPAGE_URL "https://github.com/dvm-system/tsar")

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
set(LLVM_MIN_VERSION 15.0)

include(CMakeDependentOption)
include(BCLCompilerOptions)
include(BCLUtils)
include(llvm-utility)

# There are three steps to configure LLVM and Clang:
# 1.a (PACKAGE_LLVM is OFF, GIT mono repo is only supported):
#    Use LLVM as TSAR subproject (add_subdirectory() is used to build LLVM).
# 1.b (PACKAGE_LLVM is ON):
#    Use previously installed LLVM package.
# 2. Configure LLVM project (in case of 1.a only).
# 4. Specify LLVM dependences (include directories, libraries, macros etc.

option(PACKAGE_LLVM "Use previously installed LLVM package" OFF)

set(LLVM_PROJECT_DIR "" CACHE PATH
   "Root of LLVM source tree (path to llvm-project directory, usefull if PACKAGE_LLVM is set to OFF")

set(LLVM_VERSION ${LLVM_MIN_VERSION} CACHE STRING
  "Version of installed LLVM package (useful only if PACKAGE_LLVM is set)")

option(BUILD_TSAR "Build Traits Static Analyzer" ON)
option(TSAR_SERVER "Build TSAR server shared library" OFF)

cmake_dependent_option(BUILD_CLANG "Build LLVM native C/C++/Objective-C compiler Clang" OFF
  "NOT PACKAGE_LLVM" OFF)
cmake_dependent_option(BUILD_PROFILE "Build profile runtime" OFF
  "NOT PACKAGE_LLVM" OFF)
cmake_dependent_option(BUILD_OPT "Build LLVM optimizer" OFF
  "NOT PACKAGE_LLVM" OFF)
cmake_dependent_option(BUILD_LLC "Build LLVM IR static compiler" OFF
  "NOT PACKAGE_LLVM" OFF)
cmake_dependent_option(BUILD_LINK "Build LLVM bitcode linker" OFF
  "NOT PACKAGE_LLVM" OFF)
cmake_dependent_option(BUILD_FLANG "Build LLVM native Fortran compiler Flang" OFF
  "NOT PACKAGE_LLVM" OFF)

option(TSAR_ENABLE_LLVM_DUMP "Enable use of dump() method for LLVM types" ON)

set(LLVM_SOURCE_DIR "${LLVM_PROJECT_DIR}/llvm")
set(CLANG_SOURCE_DIR "${LLVM_PROJECT_DIR}/clang")
set(FLANG_SOURCE_DIR "${LLVM_PROJECT_DIR}/flang")
set(MLIR_SOURCE_DIR "${LLVM_PROJECT_DIR}/mlir")
set(COMPILER_RT_SOURCE_DIR "${LLVM_PROJECT_DIR}/compiler-rt")

set(LLVM_BINARY_DIR "${CMAKE_BINARY_DIR}/llvm-build")
set(CLANG_BINARY_DIR "${LLVM_BINARY_DIR}/tools/clang")
set(FLANG_BINARY_DIR "${LLVM_BINARY_DIR}/tools/flang")
set(MLIR_BINARY_DIR "${LLVM_BINARY_DIR}/tools/mlir")

set(FLANG_FOUND OFF)

if(PACKAGE_LLVM)
  find_package(LLVM ${LLVM_VERSION} REQUIRED CONFIG)
  set(LLVM_SOURCE_DIR "${LLVM_BINARY_DIR}/include/llvm")
  set(CLANG_SOURCE_DIR "${LLVM_BINARY_DIR}/include/clang")
  set(FLANG_SOURCE_DIR "${LLVM_BINARY_DIR}/include/flang")
  set(MLIR_SOURCE_DIR "${LLVM_BINARY_DIR}/include/mlir")
  set(CLANG_INCLUDE_DIR "${LLVM_BINARY_DIR}/lib/clang/${LLVM_VERSION}/include")
  set(CLANG_EXECUTABLE "${LLVM_BINARY_DIR}/bin/clang${CMAKE_EXECUTABLE_SUFFIX}")
  if (EXISTS ${FLANG_SOURCE_DIR})
    set(FLANG_FOUND ON)
  else()
    message(WARNING "Flang compiler is not found, build with limited Fortran support")
  endif()
  if (MSVC_IDE)
    if (TARGET intrinsics_gen)
      set_target_properties(intrinsics_gen PROPERTIES FOLDER "Tablegenning")
    endif()
  endif()
else()
  if (NOT LLVM_PROJECT_DIR)
    message(FATAL_ERROR "Unable to find LLVM sources. \
                         Set LLVM_PROJECT_DIR to build LLVM from sources or \
                         use PACKAGE_LLVM option to access previously installed LLVM package.")
  endif()
  if(NOT EXISTS ${LLVM_SOURCE_DIR})
    message(FATAL_ERROR "LLVM_SOURCE_DIR '${LLVM_SOURCE_DIR}' does not exist")
  endif()
  if ((BUILD_CLANG OR BUILD_TSAR) AND NOT EXISTS ${CLANG_SOURCE_DIR})
    message(FATAL_ERROR "CLANG_SOURCE_DIR '${CLANG_SOURCE_DIR}' does not exist")
  endif()
  if (BUILD_FLANG AND NOT EXISTS ${FLANG_SOURCE_DIR})
    message(FATAL_ERROR "FLANG_SOURCE_DIR '${FLANG_SOURCE_DIR}' does not exist")
  endif()
  if (BUILD_FLANG AND NOT EXISTS ${MLIR_SOURCE_DIR})
    message(FATAL_ERROR "MLIR_SOURCE_DIR '${MLIR_SOURCE_DIR}' does not exist")
  endif()
  if (BUILD_PROFILE AND NOT EXISTS ${COMPILER_RT_SOURCE_DIR})
    message(FATAL_ERROR "COMPILER_RT_SOURCE_DIR '${COMPILER_RT_SOURCE_DIR}' does not exist")
  endif()
  set(LLVM_STATUS "Configuring LLVM project")
  message(STATUS ${LLVM_STATUS})
  set(LLVM_PROJECTS clang)
  if (BUILD_FLANG)
    set(LLVM_PROJECTS ${LLVM_PROJECTS} flang mlir)
  endif()
  if (BUILD_PROFILE)
    set(LLVM_PROJECTS ${LLVM_PROJECTS} compiler-rt)
  endif()
  set(LLVM_ENABLE_PROJECTS ${LLVM_PROJECTS} CACHE STRING "" FORCE)
  unset(LLVM_DIR CACHE)
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
  if (BUILD_FLANG)
    set(FLANG_FOUND ON)
  endif()
  set(TSAR_INCLUDE_PATH "lib/clang/${LLVM_VERSION}/include")
  add_custom_target(tsar-resource-headers ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_PROPERTY:clang-resource-headers,RUNTIME_OUTPUT_DIRECTORY>"
                                               "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TSAR_INCLUDE_PATH}"
    COMMENT "Copy header files to ${TSAR_INCLUDE_PATH}...")
  add_dependencies(tsar-resource-headers clang-resource-headers)
  set_target_properties(tsar-resource-headers PROPERTIES
    FOLDER Misc
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TSAR_INCLUDE_PATH}")
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TSAR_INCLUDE_PATH}
    DESTINATION ${TSAR_INCLUDE_PATH}/.. FILES_MATCHING PATTERN *.h)
  set(CLANG_INCLUDE_DIR "${LLVM_BINARY_DIR}/$<CONFIG>/lib/clang/${LLVM_VERSION}/include")
  set(CLANG_EXECUTABLE "${LLVM_BINARY_DIR}/$<CONFIG>/bin/clang${CMAKE_EXECUTABLE_SUFFIX}")
  if (BUILD_PROFILE)
    string(REGEX REPLACE "[A-Za-z].*" "" TSAR_LLVM_VERSION ${LLVM_VERSION})
    set(TSAR_LIBRARY_PATH "lib/clang/${TSAR_LLVM_VERSION}/lib")
    add_custom_target(tsar-resource-profile ALL
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${LLVM_BINARY_DIR}/$<CONFIG>/${TSAR_LIBRARY_PATH}"
                                                 "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TSAR_LIBRARY_PATH}"
      COMMENT "Copy runtime libraries to ${TSAR_LIBRARY_PATH}...")
    add_dependencies(tsar-resource-profile compiler-rt)
    set_target_properties(tsar-resource-profile PROPERTIES
      FOLDER Misc
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${TSAR_LIBRARY_PATH}")
  endif()
  if (MSVC)
    include(BCLPatch)
    bcl_patch(PATCHFILE ${PROJECT_SOURCE_DIR}/patches/llvm-${LLVM_VERSION_MAJOR}.patch
      TARGET ${LLVM_PROJECT_DIR} "" IGNORE "" PATCH_OPTIONS -p0)
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
  tablegen target transformutils coverage mcparser option debuginfodwarf
  frontendopenacc lto windowsdriver)

llvm_map_components_to_libnames(LLVM_LIBS ${LLVM_COMPONENTS})

# Add targets to the list of necessary LLVM libraries (LLVM_LIBS).
llvm_map_components_to_libnames(LLVM_TARGETS_LIBS ${LLVM_TARGETS_TO_BUILD})
foreach(L ${LLVM_TARGETS_LIBS})
  string(TOLOWER ${L} TO_COMPARE)
  string(REGEX MATCH "(asmparser|info|codegen|desc)$" FIND_RES ${TO_COMPARE})
  if (FIND_RES)
    list(APPEND LLVM_LIBS "${L}")
  endif()
endforeach()

set(CLANG_LIBS
  clangTooling clangCodeGen clangFrontend clangDriver clangSerialization
  clangParse clangSema clangAnalysis clangRewrite clangEdit clangAST clangLex
  clangBasic clangASTMatchers clangFormat clangToolingCore clangToolingInclusions
  clangSupport)

if(FLANG_FOUND)
  set(FLANG_LIBS
    flangFrontend FortranLower FortranSemantics FortranEvaluate FortranParser
    FortranDecimal FortranCommon FIRCodeGen FIRTransforms FIRBuilder
    FIRDialect FIRSupport MLIROpenMPToLLVM MLIROpenMPToLLVMIRTranslation
    MLIRLLVMToLLVMIRTranslation MLIROpenACCDialect MLIROpenMPDialect
    MLIRSCFToControlFlow MLIRFuncToLLVM MLIRArithmeticToLLVM MLIRControlFlowToLLVM
    MLIRTargetLLVMIRExport MLIRDLTIDialect MLIRLLVMIRTransforms MLIRMemRefToLLVM
    MLIRParser MLIRSCFDialect MLIRBufferizationDialect MLIRAffineDialect
    MLIRVectorDialect MLIRMemRefDialect MLIRVectorInterfaces MLIRLLVMCommonConversion
    MLIRTransforms MLIRLLVMDialect MLIRFuncDialect MLIRControlFlowDialect
    MLIRTensorDialect MLIRArithmeticUtils MLIRComplexDialect
    MLIRArithmeticDialect MLIRDialectUtils MLIRTransformUtils MLIRRewrite
    MLIRPDLToPDLInterp MLIRPDLInterpDialect MLIRPDLDialect MLIRPass MLIRAnalysis
    MLIRCallInterfaces MLIRControlFlowInterfaces MLIRInferTypeOpInterface
    MLIRSideEffectInterfaces MLIRDataLayoutInterfaces MLIRViewLikeInterface
    MLIRInferIntRangeInterface MLIRIR MLIRSupport)
  include(TestBigEndian)
  test_big_endian(IS_BIGENDIAN)
  if (IS_BIGENDIAN)
    add_compile_definitions(FLANG_BIG_ENDIAN=1)
  else()
    add_compile_definitions(FLANG_LITTLE_ENDIAN=1)
  endif()
endif()

if(NOT PACKAGE_LLVM)
  list(APPEND LLVM_INCLUDE_DIRS
    ${CLANG_SOURCE_DIR}/include ${CLANG_BINARY_DIR}/include)
endif()
if(NOT PACKAGE_LLVM AND FLANG_FOUND)
  list(APPEND LLVM_INCLUDE_DIRS
    ${FLANG_SOURCE_DIR}/include ${FLANG_BINARY_DIR}/include
    ${MLIR_SOURCE_DIR}/include ${MLIR_BINARY_DIR}/include)
# TODO: LLVM specify include directories relative to build path, however
# in case of SAPFOR build path is not a build path for LLVM.
# Hence we manually add correct include directories for Flang libraries.
  target_include_directories(obj.FIRBuilder SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_include_directories(obj.FIRCodeGen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_include_directories(obj.FIRDialect SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_include_directories(obj.FIRSupport SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_include_directories(obj.FIRTransforms SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_include_directories(obj.flangFrontend SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_include_directories(obj.flangFrontendTool SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_include_directories(obj.FortranLower SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_include_directories(obj.FortranEvaluate SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_include_directories(flang-new SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
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
  if (MSVC_VERSION GREATER_EQUAL 1920)
    add_compile_options("/Zc:__cplusplus")
  endif()
endif()

foreach(C ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER ${C} SUFFIX)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${SUFFIX} "${CMAKE_CURRENT_BINARY_DIR}/${C}/bin")
endforeach()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

add_subdirectory(utils/TableGen)
add_subdirectory(lib tsar)
add_subdirectory(tools)
add_subdirectory(test)

set_target_properties(${TSAR_TABLEGEN} PROPERTIES FOLDER "Tablegenning")

# Build Config-file Package configuration.
export(EXPORT TSARExports
  FILE ${CMAKE_CURRENT_BINARY_DIR}/TSARExports.cmake)
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/TSARConfigVersion.cmake"
  COMPATIBILITY AnyNewerVersion)

get_property(TSAR_LLVM_TOOLS GLOBAL PROPERTY TSAR_LLVM_TOOLS)
set_property(GLOBAL APPEND PROPERTY PACKAGE_PREFIX_PATH ${CMAKE_CURRENT_BINARY_DIR})

set(TSAR_EXECUTABLE /bin/tsar${CMEAK_EXECUTABLE_SUFFIX})
if (TSAR_SERVER)
  set(TSAR_SERVER_EXECUTABLE /bin/tsar-server${CMEAK_EXECUTABLE_SUFFIX})
endif()

configure_file(cmake/TSARConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/TSARConfig.cmake @ONLY)

install(EXPORT TSARExports
  FILE TSARExports.cmake DESTINATION lib/cmake/tsar)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/TSARConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/TSARConfigVersion.cmake
  DESTINATION lib/cmake/tsar)
