//===------ tsar_query.h --------- Query Manager ----------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This defines a query manager that controls construction of response when
// analysis and transformation tool is launched.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_QUERY_H
#define TSAR_QUERY_H

namespace llvm {
class Pass;
class Module;
}

namespace tsar {
class TransformationContext;

/// This is a query manager that controls construction of response when analysis
/// and transformation tool is launched.
///
/// This describes default behavior of a tool. Tow ways are available to
/// override it:
/// - The simplest way is to override createInitializationPass() and
/// createFinalizationPass() methods. Passes constructed by these methods are
/// launched before and after default sequence of passes.
/// - The second way is to override whole sequence of performed passes.
class QueryManager {
public:
  /// Creates an initialization pass to respond a query.
  virtual llvm::Pass * createInitializationPass() { return nullptr; }

  /// Creates a finalization pass to respond a query.
  virtual llvm::Pass * createFinalizationPass() { return nullptr; }

  /// Analysis the specified module and transforms source file associated with
  /// it if rewriter context is specified.
  ///
  /// \attention The transformation context is going to be taken under control,
  /// so do not free it separately. Be careful if a method is overloaded,
  /// do not forget to release context.
  virtual void run(llvm::Module *M, TransformationContext *Ctx);
};
}

#endif//TSAR_QUERY_H
