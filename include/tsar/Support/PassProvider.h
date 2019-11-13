//===- PassProvider.h ----- On The Fly Passes Provider ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// There are different passes for different entities: module, function, etc. It
// is not easy to transfer results of analysis between passes of different
// levels.
//
// For example, to obtain results of function analysis from module pass
// the specified pass will be executed on the fly for the specified function.
// To perform this getAnalysis<...>(F) method can be used. But every time when
// this method is called the whole pass sequence for a function F will be
// executed. If there are function passes P1 and P2 which are not depend from
// each other and there is also a module pass P which want to access P1 and P2
// the both passes will be performed when getAnalysis() is called:
// * getAnalysis<P1>(F) -> execute P1 and P2;
// * getAnalysis<P2>(F) -> execute P1 and P2 again.
// Other problem is that a default constructor is used to create this passes in
// the pass manager so it is not possible to specify additional parameters to
// initialize a such passes.
//
// To avoid this problems a provider pass could be used.
//===----------------------------------------------------------------------===//

#ifndef TSAR_PASS_PROVIDER_H
#define TSAR_PASS_PROVIDER_H

#include <llvm/IR/Function.h>
#include <llvm/IR/LegacyPassManagers.h>
#include <llvm/Pass.h>
#include <forward_list>
#include <type_traits>
#include <bcl/cell.h>
#include <bcl/utility.h>

namespace llvm {
class Function;
}

namespace {
/// \brief This per-function pass provides results of specified analyzes.
///
/// This pass enable access to function passes from a module pass without
/// re-execution of function passes.
///
/// To access passes P1 and P2 from a module pass P do the following.
///
/// 1. Declare a provider pass in the anonymous namespace in a .cpp file
///   a. `typedef FunctionPassProvider<P1, P2> SimpleProviderPass;`
///   b. If some additional passes (for example special implementation of
///   analysis group) must be perform before the provider pass, inherit the pass:
///   \code
///   namespace {
///   class SimplePassProviderPass: public FunctionPassProvider<P1, P2> {
///     void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
///        auto P = createSomePass();
///        AU.addRequiredID(P->getPassID());
///        delete P;
///        FunctionPassProvider::getAnalysisUsage(AU);
///     }
///   };
///   }
///   \endcode
///
/// 2. Enable implicit initialization of provider pass and passes P1 and P2.
/// \code
/// INITIALIZE_PROVIDER_BEGIN("simple-p", "Simple Provider", SimpleProviderPass)
/// INITIALIZE_PASS_DEPENDENCY(P1)
/// INITIALIZE_PASS_DEPENDENCY(P2)
/// INITIALIZE_PROVIDER_END("simple-p", "Simple Provider", SimpleProviderPass)
///
/// 3. Add SimpleProviderPass to a list of dependencies for a module pass P:
/// use `INTIALIZE_PASS_DEPENDENCY(SimpleProviderPass)` and also add
/// `AU.addRequired<SimpleProviderPass>();` in getAnalysisUsage() method.
///
/// 4. Perform initialization and execution of necessary passes from
/// runOnModule() method and access results.
/// \code
/// bool runOnModule(Module &M) {
///   ...
///   // Additional initialization of P1.
///   SimpleProviderPass::initialize<P1>(/*some function, for example lambda*/)
///   // Execute P1 and P2 for the function F.
///   auto &Provider = getAnalysis<SimpleProviderPass>(F);
///   // Access results.
///   auto &Result1 = Provider.get<P1>();
///   ...
/// }
/// \endcode
template<class... Analysis>
class FunctionPassProvider : public llvm::FunctionPass, bcl::Uncopyable {
  /// List of registered providers.
  using ProviderListT = llvm::SmallVector<FunctionPassProvider *, 6>;

  using AnalysisMap =
    bcl::StaticTypeMap<typename std::add_pointer<Analysis>::type...>;

  struct AddRequiredFunctor {
    AddRequiredFunctor(llvm::AnalysisUsage &AU) : mAU(AU) {}
    template<class AnalysisType> void operator()() {
      mAU.addRequired<typename std::remove_pointer<AnalysisType>::type>();
    }
  private:
    llvm::AnalysisUsage &mAU;
  };

  struct InitializeFunctor {
    template<class AnalysisType> void operator()(AnalysisType &Anls) {
      Anls = nullptr;
    }
  };

  struct GetRequiredFunctor {
    GetRequiredFunctor(FunctionPassProvider *Provider) : mProvider(Provider) {}
    template<class AnalysisType> void operator()(AnalysisType &Anls) {
      Anls = &mProvider->getAnalysis<
        typename std::remove_pointer<AnalysisType>::type>();
    }
  private:
    FunctionPassProvider *mProvider;
  };

  template<class RequiredType>
  struct GetWithIDFunctor {
    template <class AnalysisType> void operator()(AnalysisType *A) {
      if (ID == &std::remove_pointer<decltype(A)>::type::ID)
        Result = static_cast<RequiredType>(A);
    }
    llvm::AnalysisID &ID;
    RequiredType &Result;
  };


public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// \brief Initializes AnalysisType pass before execution.
  ///
  /// It is not necessary to call this function if there is no special
  /// initializations that must be performed for the pass AnalysisType.
  /// \pre Analysis passes had to be created. The most safe place to call this
  /// function is a runOnModule() function of a module pass that want to access
  /// results of provided analyzes.
  /// \post AnalysisType passes will be initialized for all existing providers
  /// of the current type.
  template<class AnalysisType, class Function>
  static void initialize(Function F) {
    assert(!ProviderList.empty() && "At least one provider must be created!");
    for (auto *Provider : ProviderList) {
      auto Resolver = Provider->getResolver();
      auto &PM = Resolver->getPMDataManager();
      auto P = PM.findAnalysisPass(
        static_cast<void *>(&AnalysisType::ID), true);
      assert(P && "Try to initialize a pass which is not required!");
      F(static_cast<AnalysisType &>(*P));
    }
  }

  /// \brief Default constructor.
  ///
  /// This pass is not initialized in constructor and must be initialized
  /// separately. But this should not cause a problem because a provider pass
  /// is always used as required pass for some other pass so it is
  /// initialized when dependencies is described:
  /// \code
  /// INITIALIZE_PASS_BEGIN(...)
  /// ...
  /// INITIALIZE_PASS_DEPENDENCY(SomeProviderPass)
  /// INITIALIZE_PASS_END(...)
  /// \endcode
  /// \pre There is no constructed providers.
  /// \attention At the same time only one provider is available.
  FunctionPassProvider() : FunctionPass(ID) {
    InitializeFunctor Initialize;
    mPasses.for_each(Initialize);
    ProviderList.push_back(this);
  }

  /// Destructor.
  ~FunctionPassProvider() {
    auto *This = this;
    llvm::erase_if(ProviderList,
      [This](FunctionPassProvider *FPP) { return FPP == This; });
  }

  /// Run all required analyzes and make these analyzes available with get()
  /// method.
  bool runOnFunction(llvm::Function &F) override {
    GetRequiredFunctor GetRequired(this);
    mPasses.for_each(GetRequired);
    return false;
  }

  /// Specifies that all analyzes declared as template parameters of this pass
  /// are required to be performed before execution of the pass.
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AddRequiredFunctor AddRequired(AU);
    AnalysisMap::for_each_key(AddRequired);
    AU.setPreservesAll();
  }

  /// \brief Returns the analysis information.
  ///
  /// \pre
  /// - AnalysisType must be an element of Analysis... template parameter of
  /// this pass.
  /// - Required analyzes had to be performed (isAnalysisAvailable() returns
  /// `true`).
  template<class AnalysisType> AnalysisType & get() const {
    assert((mPasses.template value<
      typename std::add_pointer<AnalysisType>::type>()) &&
      "Analysis pass is not available!");
    return *mPasses.template value<
      typename std::add_pointer<AnalysisType>::type>();
  }

  /// Return result of analysis with a specified ID.
  template<class AnalysisType = void>
  void *getWithID(llvm::AnalysisID ID) const {
    AnalysisType *Result = nullptr;
    mPasses.for_each(GetWithIDFunctor<AnalysisType *>{ID, Result});
    return Result;
  }

private:
  static ProviderListT ProviderList;
  AnalysisMap mPasses;
};

template<class... Analysis>
char FunctionPassProvider<Analysis...>::ID = 0;

template <class... Analysis>
typename FunctionPassProvider<Analysis...>::ProviderListT
  FunctionPassProvider<Analysis...>::ProviderList;
}

#define INITIALIZE_PROVIDER(passName, arg, name) \
namespace llvm { \
static void initialize##passName##Pass(PassRegistry &Registry); \
} \
INITIALIZE_PASS(passName, arg, name, true, true)

#define INITIALIZE_PROVIDER_BEGIN(passName, arg, name) \
namespace llvm { \
static void initialize##passName##Pass(PassRegistry &Registry); \
} \
INITIALIZE_PASS_BEGIN(passName, arg, name, true, true)

#define INITIALIZE_PROVIDER_END(passName, arg, name) \
INITIALIZE_PASS_END(passName, arg, name, true, true)

namespace tsar {
namespace detail {
/// Helpful function declaration to recognize passes inherited from provider.
template <class... Analysis>
bcl::TypeList<Analysis...>
check_pass_provider(FunctionPassProvider<Analysis...> &&);
}

/// Similar to std::enable_if with condition evaluated to true if P is provider.
template <class P, class T = void,
          class = decltype(detail::check_pass_provider(std::declval<P>()))>
using enable_if_pass_provider = T;

/// Provide constant value equal to `true` if `T` is a provider.
template <class T, class Enable = void>
struct is_pass_provider : std::false_type {};

/// Provide constant value equal to `true` if `T` is a provider.
template <class T>
struct is_pass_provider<T, enable_if_pass_provider<T>> : std::true_type {};

/// Provide a list (`bcl::TypeList<...>`) of analysis for a provider of type T.
template<class T>
using pass_provider_analysis =
    decltype(detail::check_pass_provider(std::declval<T>()));
}

#endif//TSAR_PASS_PROVIDER_H

