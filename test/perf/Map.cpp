//===--- Map.cpp -------------- Map Benchmark -------------------*- C++ -*-===//
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
// This benchmark compares different implementation of maps.
//
//===----------------------------------------------------------------------===//

#include <tsar/Core/tsar-config.h>
#include <tsar/ADT/PersistentMap.h>
#include <tsar/ADT/Bimap.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/raw_ostream.h>
#include <chrono>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <vector>

/// This macro determines size of key (KEY_SIZE + 1) * std::size_t.
/// If it is not defined std::size_t is used.
//#define KEY_SIZE 1

/// This macro determine size of value (VALUE_SIZE + 1) * std::size_t.
/// If it is not defined std::size_t is used.
#define VALUE_SIZE 5

using namespace llvm;
using namespace tsar;

template<class T, unsigned N>
struct DataType {
  T Value = 0;
  T Stab[N];
  DataType() = default;
  DataType(T V) : Value(V) {}
  DataType & operator+=(const DataType &RHS) {
    Value += RHS.Value;
    return *this;
  }
  operator T() const { return Value; }
};

template<class T> struct HashInfo : public std::hash<T> {};
template<class T, unsigned N>
struct HashInfo<DataType<T, N>> : public std::hash<T> {};

template<class T> struct MapInfo : public llvm::DenseMapInfo<T> {};
template<class T, unsigned N>
struct MapInfo<DataType<T, N>> : public llvm::DenseMapInfo<T> {};

#if defined KEY_SIZE
using KeyT = DataType<std::size_t, KEY_SIZE>;
#else
using KeyT = std::size_t;
#endif

#if defined VALUE_SIZE
using ValueT = DataType<std::size_t, VALUE_SIZE>;
#else
using ValueT = std::size_t;
#endif

using AccumulateDataT = std::size_t;
using DataSetT = std::vector<KeyT>;
using TimeT = std::chrono::duration<double>;

static inline KeyT initializeData(std::size_t Idx) noexcept {
  return Idx;
}

DataSetT initializeDataSet(std::size_t Size) {
  DataSetT Data(Size);
  for (std::size_t I = 0; I < Size; ++I)
    Data[I] = initializeData(I);
  // Mix up a set of data.
  for (std::size_t I = 0; I < Size; ++I) {
     std::size_t J = std::rand() % Size;
     auto Tmp = Data[I];
     Data[I] = Data[J];
     Data[J] = Tmp;
  }
  return Data;
}

#define EMPLACE_TIME(emplace_) \
template<class MapT, class ListT> \
TimeT emplace_##Time(unsigned AccumulateMaxIter, const DataSetT &D, \
    MapT &M, ListT &L) { \
  auto Start = std::chrono::high_resolution_clock::now(); \
  for (std::size_t I = 0, EI = D.size(); I < EI; ++I) {\
    auto Itr = M.emplace_(D[I], D[I]).first; \
    assert(Itr != M.end() && "Invalid iterator!"); \
    for (std::size_t J = 0; J < AccumulateMaxIter; ++J) \
      L.push_back(Itr); \
  } \
  auto End = std::chrono::high_resolution_clock::now(); \
  std::chrono::duration<double> Diff = End - Start; \
  return End - Start; \
}

EMPLACE_TIME(emplace)
EMPLACE_TIME(try_emplace)

#define ERASE_TIME(erase_) \
template<class MapT> \
TimeT erase_##Time(std::size_t Size, MapT &M) { \
  auto Start = std::chrono::high_resolution_clock::now(); \
  for (std::size_t I = 0; I < Size; ++I) \
    M.erase_(initializeData(I)); \
  auto End = std::chrono::high_resolution_clock::now(); \
  return End - Start; \
}

ERASE_TIME(erase)
ERASE_TIME(erase_first)
ERASE_TIME(erase_second)

#define FIND_TIME(find_) \
template<class MapT> \
TimeT find_##Time(std::size_t Size, const MapT &M, AccumulateDataT &Sum) { \
  auto Start = std::chrono::high_resolution_clock::now(); \
  for (std::size_t I = 0; I < Size; ++I) { \
    auto Itr = M.find_(initializeData(I)); \
    if (Itr != M.end()) \
      Sum += Itr->first; \
  } \
  auto End = std::chrono::high_resolution_clock::now(); \
  return End - Start; \
}

FIND_TIME(find)
FIND_TIME(find_first)
FIND_TIME(find_second)

void accumulateDataSet(unsigned AccumulateMaxIter, const DataSetT &D,
    AccumulateDataT &Sum) {
  AccumulateDataT Tmp{ 0 };
  std::size_t Size = D.size();
  for (unsigned J = 0; J < AccumulateMaxIter; ++J)
    for (unsigned K = 0; K < Size; ++K)
      Tmp += D[K];
  Sum += Tmp / AccumulateMaxIter;
}

#define ACCUMULATE_TIME(accumulate_, find_) \
template<class MapT> TimeT accumulate_##Time(unsigned AccumulateMaxIter, \
    std::size_t Size, const MapT &M, AccumulateDataT &Sum) { \
  TimeT Find; \
  AccumulateDataT Tmp{ 0 }; \
  for (unsigned J = 0; J < AccumulateMaxIter; ++J) \
    Find += find_##Time(Size, M, Tmp); \
  Sum += Tmp / AccumulateMaxIter; \
  return Find; \
}

ACCUMULATE_TIME(accumulate, find)
ACCUMULATE_TIME(accumulate_first, find_first)
ACCUMULATE_TIME(accumulate_second, find_second)

void run(std::size_t Size,
    unsigned MaxIter = 5, unsigned AccumulateMaxIter = 10) {
  TimeT EmplaceSM(0), TryEmplacePM(0), TryEmplacePMP(0), TryEmplaceDM(0),
    EmplaceSUM(0), EmplaceBM(0);
  TimeT EraseSM(0), ErasePM(0), ErasePMP(0), EraseDM(0),
    EraseSUM(0), EraseBM(0);
  TimeT FindSM(0), FindPM(0), FindPMP(0), FindDM(0), FindSUM(0), FindBM(0);
  AccumulateDataT Sum{ 0 }, SumSM{ 0 }, SumPM{ 0 }, SumPMP{ 0 }, SumDM{ 0 };
  AccumulateDataT SumSUM{ 0 }, SumBM{ 0 };
  auto Data = initializeDataSet(Size);
  accumulateDataSet(AccumulateMaxIter, Data, Sum);
  Sum *= MaxIter;
  Sum += AccumulateMaxIter * MaxIter * Size;
  for (int I = 0; I < MaxIter; ++I) {
      std::map<KeyT, ValueT> SM;
      std::vector<decltype(SM)::iterator> SML;
      EmplaceSM += emplaceTime(AccumulateMaxIter, Data, SM, SML);
      FindSM += accumulateTime(AccumulateMaxIter, Size, SM, SumSM);
      SumSM += SML.size();
      EraseSM += eraseTime(Size, SM);
      SML.clear();
      std::unordered_map<KeyT, ValueT, HashInfo<KeyT>> SUM;
      std::vector<decltype(SUM)::iterator> SUML;
      EmplaceSUM += emplaceTime(AccumulateMaxIter, Data, SUM, SUML);
      FindSUM += accumulateTime(AccumulateMaxIter, Size, SUM, SumSUM);
      SumSUM += SUML.size();
      EraseSUM += eraseTime(Size, SUM);
      SUML.clear();
      Bimap<KeyT, ValueT, MapInfo<KeyT>, MapInfo<ValueT>> BM;
      std::vector<decltype(BM)::iterator> BML;
      EmplaceBM += emplaceTime(AccumulateMaxIter, Data, BM, BML);
      FindBM += accumulate_firstTime(AccumulateMaxIter, Size, BM, SumBM);
      SumBM += BML.size();
      EraseBM += erase_firstTime(Size, BM);
      BML.clear();
      PersistentMap<KeyT, ValueT, MapInfo<KeyT>> PM;
      std::vector<decltype(PM)::iterator> PML;
      TryEmplacePM += try_emplaceTime(AccumulateMaxIter, Data, PM, PML);
      FindPM += accumulateTime(AccumulateMaxIter, Size, PM, SumPM);
      SumPM += PML.size();
      ErasePM += eraseTime(Size, PM);
      PML.clear();
      PersistentMap<KeyT, ValueT, MapInfo<KeyT>> PMP;
      std::vector<decltype(PMP)::persistent_iterator> PMPL;
      TryEmplacePMP += try_emplaceTime(AccumulateMaxIter, Data, PMP, PMPL);
      FindPMP += accumulateTime(AccumulateMaxIter, Size, PMP, SumPMP);
      SumPMP += PMPL.size();
      ErasePMP += eraseTime(Size, PMP);
      PMPL.clear();
      DenseMap<KeyT, ValueT, MapInfo<KeyT>> DM;
      std::vector<decltype(DM)::iterator> DML;
      TryEmplaceDM += try_emplaceTime(AccumulateMaxIter, Data, DM, DML);
      FindDM += accumulateTime(AccumulateMaxIter, Size, DM, SumDM);
      SumDM += DML.size();
      EraseDM += eraseTime(Size, DM);
      DML.clear();
  }
  outs() << "Results for " << __FILE__ << " benchmark\n";
  outs() << "  date " << __DATE__ << "\n";
  outs() << "  compiler ";
#if defined __GNUC__
  outs() << "GCC " << __GNUC__;
#elif defined __clang__
  outs() << "Clang " << __clang__;
#elif defined _MSC_VER
  outs() << "Microsoft " << _MSC_VER;
#else
  outs() << "unknown";
#endif
  outs() << "\n";
  outs() << "  LLVM version " << LLVM_VERSION_STRING << "\n";
  outs() << "  TSAR version " << TSAR_VERSION_STRING << "\n";
  outs() << "  size of data " << Size << "\n";
  outs() << "  key size " << sizeof(KeyT) << "\n";
  outs() << "  value size " << sizeof(ValueT) << "\n";
  outs() << "  number of iterations " << MaxIter << "\n";
  outs() << "  number of reduction iterations " << AccumulateMaxIter << "\n";
  outs() << "  accumulated sum " << Sum << "\n";
  outs() << "\n";
  if (SumSM == Sum)
    outs() << "  std::map accumulated sum is correct\n";
  else
    outs() << "  std::map accumulated sum is NOT correct (difference "
      << (Sum > SumSM ? Sum - SumSM : SumSM - Sum) << ")\n";
  if (SumSUM == Sum)
    outs() << "  std::unordered_map accumulated sum is correct\n";
  else
    outs() << "  std::unordered_map accumulated sum is NOT correct (difference "
      << (Sum > SumSUM ? Sum - SumSUM : SumSUM - Sum) << ")\n";
  if (SumDM == Sum)
    outs() << "  llvm::DenseMap accumulated sum is correct\n";
  else
    outs() << "  llvm::DenseMap accumulated sum is NOT correct (difference "
      << (Sum > SumDM ? Sum - SumDM : SumDM - Sum) << ")\n";
  if (SumPM == Sum)
    outs() << "  tsar::PersistentMap (without persistent) accumulated sum is"
      " correct\n";
  else
    outs() << "  tsar::PersistentMap (without persistent) accumulated sum is"
      " NOT correct (difference "
      << (Sum > SumPM ? Sum - SumPM : SumPM - Sum) << ")\n";
  if (SumPMP == Sum)
    outs() << "  tsar::PersistentMap accumulated sum is correct\n";
  else
    outs() << "  tsar::PersistentMap accumulated sum is NOT correct (difference "
      << (Sum > SumPMP ? Sum - SumPMP : SumPMP - Sum) << ")\n";
  if (SumBM == Sum)
    outs() << "  tsar::Bimap accumulated sum is correct\n";
  else
    outs() << "  tsar::Bimap accumulated sum is NOT correct (difference "
      << (SumBM > Sum ? Sum - SumBM : SumBM - Sum) << ")\n";
  outs() << "\n";
  std::map<double, std::string> Time;
  Time.emplace((EmplaceSM / MaxIter).count(),
    "  std::map emplace() time (.s) ");
  Time.emplace((EmplaceSUM / MaxIter).count(),
    "  std::unordered_map emplace() time (.s) ");
  Time.emplace((TryEmplaceDM / MaxIter).count(),
    "  llvm::DenseMap try_emplace() time (.s) ");
  Time.emplace((TryEmplacePM / MaxIter).count(),
    "  tsar::PersistentMap (without persistent) try_emplace() time (.s) ");
  Time.emplace((TryEmplacePMP / MaxIter).count(),
    "  tsar::PersistentMap try_emplace() time (.s) ");
  Time.emplace((EmplaceBM / MaxIter).count(),
    "  tsar::Bimap emplace() time (.s) ");
  for (auto &T : Time)
    outs() << T.second << T.first << "\n";
  outs() << "\n";
  Time.clear();
  Time.emplace((FindSM / MaxIter).count(),
    "  std::map find() time (.s) ");
  Time.emplace((FindSUM / MaxIter).count(),
    "  std::unordered_map find() time (.s) ");
  Time.emplace((FindDM / MaxIter).count(),
    "  llvm::DenseMap find() time (.s) ");
  Time.emplace((FindPM / MaxIter).count(),
    "  tsar::PersistentMap (without persistent) find() time (.s) ");
  Time.emplace((FindPMP / MaxIter).count(),
    "  tsar::PersistentMap find() time (.s) ");
  Time.emplace((FindBM / MaxIter).count(),
    "  tsar::Bimap find_first() time (.s) ");
  for (auto &T : Time)
    outs() << T.second << T.first << "\n";
  outs() << "\n";
  Time.clear();
  Time.emplace((EraseSM / MaxIter).count(),
    "  std::map erase() time (.s) ");
  Time.emplace((EraseSUM / MaxIter).count(),
    "  std::unordered_map erase() time (.s) ");
  Time.emplace((EraseDM / MaxIter).count(),
    "  llvm::DenseMap erase() time (.s) ");
  Time.emplace((ErasePM / MaxIter).count(),
    "  tsar::PersistentMap (without persistent) erase() time (.s) ");
  Time.emplace((ErasePMP / MaxIter).count(),
    "  tsar::PersistentMap erase() time (.s) ");
  Time.emplace((EraseBM / MaxIter).count(),
    "  tsar::Bimap erase_first() time (.s) ");
  for (auto &T : Time)
    outs() << T.second << T.first << "\n";
}

int main(int Argc, const char **Argv) {
  std::string Help =
    "parameter: <size of data> [number of iterations]"
    "[number reduction iterations]\n";
  if (Argc < 2) {
    errs() << "error: too few arguments\n" << Help;
    return 1;
  } else if (Argc > 4) {
    errs() << "error: too many arguments\n" << Help;
    return 2;
  }
  std::size_t Size = std::atoll(Argv[1]);
  unsigned MaxIter = (Argc > 2) ? std::atoi(Argv[2]) : 10;
  unsigned AccumulatedMaxIter = (Argc > 3) ? std::atoi(Argv[3]) : 5;
  if (Size == 0) {
    errs() << "error: invalid size of data set\n" << Help;
    return 3;
  }
  if (MaxIter == 0) {
    errs() << "error: invalid number of iterations\n" << Help;
    return 4;
  }
  if (AccumulatedMaxIter == 0) {
    errs() << "error: invalid number of reduction iterations\n" << Help;
    return 5;
  }
  run(Size, MaxIter, AccumulatedMaxIter);
  return 0;
}
