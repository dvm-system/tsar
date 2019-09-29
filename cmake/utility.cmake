# Copy file with a patch to a specified target if a specified patch exist.
function(sapfor_configure_patch patchfile targetfile)
  if(NOT EXISTS ${patchfile})
    return()
  endif()
  configure_file(${patchfile} ${targetfile})
endfunction(sapfor_configure_patch)
