//===------ tsar_query.h ------ Loop Query Manager --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This defines a query manager to obtain results of analysis of certain loops.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_LOOP_QUERY_H
#define TSAR_LOOP_QUERY_H

#include <clang/AST/Stmt.h>
#include <llvm/ADT/DenseMap.h>
#include <utility.h>
#include "tsar_query.h"
#include "tsar_trait.h"

namespace tsar {
class LoopQueryManager : public QueryManager {
  typedef DITrait::PointerSet PointerSet;
  typedef llvm::DenseMap<clang::ForStmt *, DIDependencySet *> LoopMap;

public:
  /// \brief This type used to iterate over all analyzed loops.
  ///
  /// If analysis of a loop has been successfully performed traits of it
  /// is available as a second value in the pair.
  typedef LoopMap::const_iterator const_iterator;

  /// This type used to iterate over all analyzed pointers.
  typedef PointerSet::const_iterator pointer_iterator;

  /// Destroys the query and all responses.
  ~LoopQueryManager() {
    for (auto Pair : mLoops) {
      if (Pair.second)
        delete Pair.second;
    }
    for (auto Ptr : mPointers)
      delete Ptr;
  }

  /// Returns iterator that points to the beginning of the loops collection.
  const_iterator begin() const { return mLoops.begin(); }

  /// Returns iterator that points to the ending of the loops collection.
  const_iterator end() const { return mLoops.end(); }

  /// Returns iterator that points to the beginning of the pointers collection.
  pointer_iterator pointer_begin() const { return mPointers.begin(); }

  /// Returns iterator that points to the ending of the pointers collection.
  pointer_iterator pointer_end() const { return mPointers.end(); }

  /// Specifies that a loop should be analyzed.
  void addTarget(clang::ForStmt *S) {
    assert(S && "Loop must not be null!");
    mLoops.insert(std::make_pair(S, new DIDependencySet));
  }

  /// Returns traits recognized for the specified loop.
  const DIDependencySet & getResponse(clang::ForStmt *S) {
    assert(S && "Loop must not be null!");
    assert(mLoops.count(S) && "Traits for the loop must be specified!");
    return *mLoops.find(S);
  }

private:
  LoopMap mLoops;
  PointerSet mPointers;
};
}
#endif//TSAR_LOOOP_QUERY_H

