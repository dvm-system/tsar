//===- Intrinsics.td - Defines all LLVM intrinsics ---------*- tablegen -*-===//
//
//                      Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines properties of all TSAR intrinsics.
//
//===----------------------------------------------------------------------===//

// Kind of a type
class TypeKind;

def Void      : TypeKind;
def Any       : TypeKind;
def Pointer   : TypeKind;
def Size      : TypeKind;
def Int	      : TypeKind;

class Type<TypeKind kind> {
  TypeKind Kind = kind;
}

def tsar_void_ty : Type<Void>;
def tsar_any_ty : Type<Any>;
def tsar_size_ty : Type<Size>;
def tsar_position_ty : Type<Size>;

class PointerType<Type elty> : Type<Pointer> {
  Type ElTy = elty;
}

def tsar_di_var_ty : PointerType<tsar_any_ty>;
def tsar_di_loc_ty : PointerType<tsar_any_ty>;
def tsar_di_loop_ty : PointerType<tsar_any_ty>;
def tsar_di_func_ty : PointerType<tsar_any_ty>;
def tsar_di_ty : PointerType<PointerType<tsar_any_ty>>;
def tsar_di_string_ty : PointerType<tsar_any_ty>;
def tsar_addr_ty : PointerType<tsar_any_ty>;
def tsar_arr_base_ty : PointerType<tsar_any_ty>;
def tsar_pool_ptr_ty : PointerType<PointerType<PointerType<tsar_any_ty>>>;
def tsar_size_ptr_ty : PointerType<tsar_size_ty>;

// Define one intrinsic.
class Intrinsic<string name,
                Type ret_type = tsar_void_ty,
                list<Type> param_types = []> {
  string Name = name;
  Type RetType = ret_type;
  list<Type> ParamTypes = param_types;
}

def reg_var : Intrinsic<"sapforRegVar",
                        tsar_void_ty,
                        [tsar_di_var_ty, tsar_addr_ty]>;

def reg_arr : Intrinsic<"sapforRegArr",
                        tsar_void_ty,
                        [tsar_di_var_ty, tsar_size_ty, tsar_addr_ty]>;

def read_var : Intrinsic<"sapforReadVar", tsar_void_ty,
                        [tsar_di_loc_ty, tsar_addr_ty, tsar_di_var_ty]>;

def read_arr : Intrinsic<"sapforReadArr", tsar_void_ty,
                        [tsar_di_loc_ty, tsar_addr_ty, tsar_di_var_ty, 
                        tsar_arr_base_ty]>;

def write_var_end : Intrinsic<"sapforWriteVarEnd", tsar_void_ty,
                        [tsar_di_loc_ty, tsar_addr_ty, tsar_di_var_ty]>;

def write_arr_end : Intrinsic<"sapforWriteArrEnd", tsar_void_ty,
                        [tsar_di_loc_ty, tsar_addr_ty, tsar_di_var_ty, 
                        tsar_arr_base_ty]>;

def func_begin : Intrinsic<"sapforFuncBegin",
                        tsar_void_ty, [tsar_di_func_ty]>;

def func_end : Intrinsic<"sapforFuncEnd",
                        tsar_void_ty, [tsar_di_func_ty]>;

def reg_dummy_var : Intrinsic<"sapforRegDummyVar", tsar_void_ty,
                        [tsar_di_func_ty, tsar_addr_ty, tsar_position_ty]>;

def reg_dummy_arr : Intrinsic<"sapforRegDummyArr", tsar_void_ty,
                        [tsar_di_func_ty, tsar_size_ty, tsar_addr_ty,
                        tsar_position_ty]>;

def func_call_begin : Intrinsic<"sapforFuncCallBegin",
                        tsar_void_ty, [tsar_di_loc_ty, tsar_di_func_ty]>;

def func_call_end : Intrinsic<"sapforFuncCallEnd",
                        tsar_void_ty, [tsar_di_func_ty]>;

def sl_begin : Intrinsic<"sapforSLBegin", tsar_void_ty,
                        [tsar_di_loop_ty, tsar_size_ty, tsar_size_ty,
                        tsar_size_ty]>;

def sl_end : Intrinsic<"sapforSLEnd",
                        tsar_void_ty, [tsar_di_loop_ty]>;

def sl_iter : Intrinsic<"sapforSLIter",
                        tsar_void_ty, [tsar_di_loop_ty, tsar_addr_ty]>;

def init_di : Intrinsic<"sapforInitDI", 
                        tsar_void_ty, 
                        [tsar_di_ty, tsar_di_string_ty, tsar_size_ty]>;

def allocate_pool : Intrinsic<"sapforAllocatePool", 
                        tsar_void_ty, 
                        [tsar_pool_ptr_ty, tsar_size_ty]>;

def decl_types : Intrinsic<"sapforDeclTypes", 
                        tsar_void_ty,
                        [tsar_size_ty, tsar_size_ptr_ty, tsar_size_ptr_ty]>;
