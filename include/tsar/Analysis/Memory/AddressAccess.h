#ifndef SAPFOR_ADDRESSACCESS_H
#define SAPFOR_ADDRESSACCESS_H

#include "llvm/Analysis/CallGraphSCCPass.h"
#include <bcl/utility.h>
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/DFRegionInfo.h"


namespace llvm {
class AddressAccessAnalyser:
public CallGraphSCCPass, private bcl::Uncopyable {
public:
    static char ID;

    AddressAccessAnalyser() : CallGraphSCCPass(ID) {
        initializeAddressAccessAnalyserPass(*PassRegistry::getPassRegistry());
    }

    bool runOnSCC(CallGraphSCC &SCC) override;

    void getAnalysisUsage(AnalysisUsage &AU) const override;

    void print(raw_ostream &OS, const Module *M) const override { };

    void releaseMemory() override { }
};
}

#endif //SAPFOR_ADDRESSACCESS_H
