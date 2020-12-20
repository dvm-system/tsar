function(tsar_test)
  # Syntax:
  # tsar_test TARGET cmake-target-name
  #
  # Generates custom commands for invoking pts.pl.
  # Corresponding targets Test${TARGET}, Init${TARGET}, Fail${TARGET} are generated.
  #
  # If ${PTS_EXECUTABLE} PTS executable does not exist do nothing.

  cmake_parse_arguments(TT "" "TARGET;PLUGIN;PASSNAME" "" ${ARGN})

  if(NOT TT_TARGET)
    message(FATAL_ERROR "TARGET name required by tsar_testing")
  endif()
  set(TT_TEST_TARGET Test${TT_TARGET})
  set(TT_INIT_TARGET Init${TT_TARGET})
  set(TT_FAIL_TARGET Fail${TT_TARGET})

  if(NOT PERL_FOUND OR NOT EXISTS "${PTS_EXECUTABLE}")
    return()
  endif()

  if(NOT TT_PASSNAME)
    set(TT_MESSAGE "Testing...")
    set(TT_INIT_MESSAGE "Initialization of tests...")
    set(TT_FAIL_MESSAGE "Running of skipped tests...")
  else()
    set(TT_MESSAGE "Testing '${TT_PASSNAME}' pass...")
    set(TT_INIT_MESSAGE "Initialization of tests for '${TT_PASSNAME}' pass...")
    set(TT_FAIL_MESSAGE "Running of skipped tests for ${TT_PASSNAME}' pass...")
  endif()

  set(OPTION_LIST --total-time --failed f -s)
  set(PLUGIN_LIST -I ${PTS_PLUGIN_PATH})
  if (WIN32)
    set(TT_PLATFORM "WINDOWS")
  else()
    set(TT_PLATFORM "NOT_WINDOWS")
  endif()
  set(TASK_CONFIG -T . -T ${PTS_SETENV_PATH} setenv:tsar=$<TARGET_FILE:tsar>,platform=${TT_PLATFORM} parallel)

  add_custom_target(${TT_TEST_TARGET}
    COMMAND ${PERL_EXECUTABLE} ${PTS_EXECUTABLE} ${OPTION_LIST} ${PLUGIN_LIST} ${TASK_CONFIG} check
    COMMENT ${TT_MESSAGE}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
  set_target_properties(${TT_TEST_TARGET} PROPERTIES FOLDER "Tsar testing")
  set_target_properties(${TT_TEST_TARGET} PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD ON)
  set_property(GLOBAL APPEND PROPERTY TSAR_TEST_TARGETS ${TT_TEST_TARGET})
  add_dependencies(${TT_TEST_TARGET} tsar)

  add_custom_target(${TT_INIT_TARGET}
    COMMAND ${PERL_EXECUTABLE} ${PTS_EXECUTABLE} ${OPTION_LIST} ${PLUGIN_LIST} ${TASK_CONFIG} init
    COMMENT ${TT_INIT_MESSAGE}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
  set_target_properties(${TT_INIT_TARGET} PROPERTIES FOLDER "Tsar testing")
  set_target_properties(${TT_INIT_TARGET} PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD ON)
  set_property(GLOBAL APPEND PROPERTY TSAR_TEST_INIT_TARGETS ${TT_INIT_TARGET})
  add_dependencies(${TT_INIT_TARGET} tsar)

  add_custom_target(${TT_FAIL_TARGET}
    COMMAND ${PERL_EXECUTABLE} ${PTS_EXECUTABLE} ${OPTION_LIST} ${PLUGIN_LIST} ${TASK_CONFIG} fail
    COMMENT ${TT_FAIL_MESSAGE}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
  set_target_properties(${TT_FAIL_TARGET} PROPERTIES FOLDER "Tsar testing")
  set_target_properties(${TT_FAIL_TARGET} PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD ON)
  set_property(GLOBAL APPEND PROPERTY TSAR_TEST_FAIL_TARGETS ${TT_FAIL_TARGET})
  add_dependencies(${TT_FAIL_TARGET} tsar)

  include(CTest)

  add_test(NAME ${TT_TARGET}
    COMMAND ${PERL_EXECUTABLE} ${PTS_EXECUTABLE} ${OPTION_LIST} ${PLUGIN_LIST} ${TASK_CONFIG} check
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
endfunction(tsar_test)
