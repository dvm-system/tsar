function(tsar_tablegen)
  # Syntax:
  # tsar_tablegen output-file [tablegen-arg ...] SOURCE source-file
  # [[TARGET cmake-target-name] [DEPENDS extra-dependency ...]]
  #
  # Generates a custom command for invoking tblgen as
  #
  # tblgen source-file -o=output-file tablegen-arg ...
  #
  # and, if cmake-target-name is provided, creates a custom target for
  # executing the custom command depending on output-file. It is
  # possible to list more files to depend after DEPENDS.

  cmake_parse_arguments(TTG "" "SOURCE;TARGET" "" ${ARGN})

  if( NOT TTG_SOURCE )
    message(FATAL_ERROR "SOURCE source-file required by tsar_tablegen")
  endif()

  set( LLVM_TARGET_DEFINITIONS ${TTG_SOURCE} )

  tablegen(TSAR ${TTG_UNPARSED_ARGUMENTS})

  if(TTG_TARGET)
    add_public_tablegen_target(${TTG_TARGET})
    set_target_properties( ${TTG_TARGET} PROPERTIES FOLDER "Tsar tablegenning")
    set_property(GLOBAL APPEND PROPERTY TSAR_TABLEGEN_TARGETS ${TTG_TARGET})
  endif()
endfunction(tsar_tablegen)