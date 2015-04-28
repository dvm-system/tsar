//===--- tsar_private.h - Private Variable Analyzer --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
//
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PRIVATE_H
#define TSAR_PRIVATE_H

namespace llvm {
class BasicBlock;
}

namespace tsar {
/// Interface to access nodes of a graph processed in the data flow analysis.
class DataFlowNode {
public:
  virtual ~DataFlowNode() {}

  /// Evaluate the transfer function according to a data flow analysis algorithm.
  virtual void evaluateTransferFunction() = 0;
};
}

#endif