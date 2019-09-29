//===- LegacyPassNameParser.h - Patch Implementation ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains fixed implementation for some of parser classes.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PATCH_LPNP_H
#define TSAR_PATCH_LPNP_H

#include <llvm/IR/LegacyPassNameParser.h>

namespace llvm {
namespace patch {
/// FilteredPassNameParser class - Make use of the pass registration
/// mechanism to automatically add a command line argument to opt for
/// each pass that satisfies a filter criteria.  Filter should return
/// true for passes to be registered as command-line options.
///
template<typename Filter>
class FilteredPassNameParser : public PassNameParser {
private:
  Filter filter;

public:
  // PATCH: This constructor is not available in the initial
  // LLVM implementation. So, this class could not be created.
  FilteredPassNameParser(cl::Option & O) : PassNameParser(O) {}

  bool ignorablePassImpl(const PassInfo *P) const override {
    return !filter(*P);
  }
};
}
}
#endif TSAR_PATCH_LPNP_H
