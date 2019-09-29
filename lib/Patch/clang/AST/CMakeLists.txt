set(PATCH_HEADERS_BASE ASTImporter)

include(utility)

set(CURRENT_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include/tsar/Patch/clang/AST)
foreach(BASE ${PATCH_HEADERS_BASE})
  sapfor_configure_patch(
    "${CURRENT_INCLUDE_DIR}/${BASE}-${LLVM_VERSION_MAJOR}.h.in"
    "${BASE}.h")
  sapfor_configure_patch(
    "${CURRENT_INCLUDE_DIR}/${BASE}-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.h.in"
    "${BASE}.h")
  sapfor_configure_patch(
    "${CURRENT_INCLUDE_DIR}/${BASE}-${LLVM_VERSION}.h.in"
    "${BASE}.h")    
endforeach()
