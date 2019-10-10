# Traits Static Analyzer (TSAR)

Traits Static Analyzer (TSAR) is a part of a system for automated parallelization SAPFOR.
The main goal of analyzer is to determine data dependences, privatizable, reduction and induction variables and other traits of analyzed program which could be helpful to parallelize program in automated way. Both scalar variables and arrays are explored.

The analyzer is based on the [LLVM] compiler infrastructure and uses the [LLVM IR] as an internal representation. This allows us implement algorithms independent from the source language of an investigated program. This approach is limited only by the necessity of having the appropriate frontend that generates appropriate LLVM IR for the source code. [Clang] is used for C/C ++ languages.

All of the code in TSAR is available under the Apache License v2.0.

In the TSAR source tree a certain coding standards is used.

The main golden rule is following ([LLVM Coding Standards][LLVM CS]):

> **If you are extending, enhancing, or bug fixing already implemented code, use the style
that is already being used so that the source is uniform and easy to follow.**

The standards are based on [LLVM Coding Standards][LLVM CS] and [Google C++ Style Guide][Google CS] and are partially described in [TSAR Wiki][TSAR Wiki].

[LLVM]: http://llvm.org/ "LLVM ompiler Infrastructure"
[LLVM IR]: http://llvm.org/docs/LangRef.html "LLVM Intermediate Representation"
[Clang]: http://clang.llvm.org/ "Clang Compiler"
[LLVM CS]: http://llvm.org/docs/CodingStandards.html "LLVM Coding Standards"
[Google CS]: https://google.github.io/styleguide/cppguide.html "Google C++ Style Guide"
[TSAR Wiki]: https://github.com/dvm-system/tsar/wiki "TSAR Wiki"
