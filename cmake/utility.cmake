# Replaces a compiler option or switch `old' in `var' by `new'.
# If `old' is not in `var', appends `new' to `var'.
# Example: llvm_replace_compiler_option(CMAKE_CXX_FLAGS_RELEASE "-O3" "-O2")
# If the option already is on the variable, don't add it.
# This code is taken from LLVM (cmake/modules/LLVMProcessSources.cmake).
function(sapfor_replace_compiler_option var old new)
  if("${${var}}" MATCHES "(^| )${new}($| )")
    set(n "")
  else()
    set(n "${new}")
  endif()
  if("${${var}}" MATCHES "(^| )${old}($| )")
    string(REGEX REPLACE "(^| )${old}($| )" " ${n} " ${var} "${${var}}")
  else()
    set(${var} "${${var}} ${n}")
  endif()
  set( ${var} "${${var}}" PARENT_SCOPE )
endfunction(sapfor_replace_compiler_option)

# Appends 'value' to a list of specified variables.
# Example: append("-std=gnu++11" CMAKE_CXX_FLAGS)
# This code is taken from LLVM (cmake/modulesHandleLLVMOptions.cmake).
function(append value)
  foreach(variable ${ARGN})
    set(${variable} "${${variable}} ${value}" PARENT_SCOPE)
  endforeach(variable)
endfunction()

# Applaies a patch 'patch' to a working in a 'source_dir'
# The optional third parameter specifies list of options for svn patch command.
# Pre: subversion package must be available.
# Post: if 'patch' and 'source_dir' exists than the specified patch 
# will be applied.
function(sapfor_patch patch source_dir)
  if(NOT EXISTS ${patch} OR NOT EXISTS ${source_dir})
    return()
  endif()  
  set(status "Applying patch ${patch} to ${source_dir}")
  if(${ARGC} LESS 3)
    set(options "")
  else()
    set(options ${ARGV2})
    set(status "${status} with options '${options}'")
  endif()
  message(STATUS ${status})    
  execute_process(COMMAND ${Subversion_SVN_EXECUTABLE} 
    patch ${options} ${patch} ${source_dir} RESULT_VARIABLE error)
  if(error)
    message(FATAL_ERROR "${status} - error")
  else()       
    message(STATUS "${status} - done")
  endif()
endfunction(sapfor_patch)