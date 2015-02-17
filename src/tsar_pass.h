/*! \file
    \brief This contains definitions that is necessary to combine TSAR and LLVM.

    At first, in this file functions are defined to create an instances of TSAR pass.
*/
#ifndef TSAR_PASS_H
#define TSAR_PASS_H

namespace llvm
{
    class FunctionPass;

    //! This creates a pass to analyze private variables.
    FunctionPass *createPrivateRecognitionPass( );
}

#endif//TSAR_PASS_H
