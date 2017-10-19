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
def tsar_int_ty : Type<Int>;
def tsar_any_ty : Type<Any>;
def tsar_size_ty : Type<Size>;
def tsar_position_ty : Type<Size>;

class PointerType<Type elty> : Type<Pointer> {
  Type ElTy = elty;
}

def tsar_di_var_ty : PointerType<tsar_any_ty>;
def tsar_di_loc_ty : PointerType<tsar_any_ty>;
def tsar_di_loop_ty : PointerType<tsar_any_ty>;
def tsar_di_funct_ty : PointerType<tsar_any_ty>;
def tdar_di_call_ty : PointerType<tsar_any_ty>;
def tsar_addr_ty : PointerType<tsar_any_ty>;
def tsar_arr_base_ty : PointerType<tsar_any_ty>;

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
                        tsar_void_ty>;
                        //[tsar_di_var_ty, tsar_size_ty, tsar_addr_ty]>;

def read_var : Intrinsic<"sapforReadVar",
                        tsar_void_ty>;
                        //[tsar_di_loc_ty, tsar_addr_ty, tsar_di_var_ty]>;

def read_arr : Intrinsic<"sapforReadArr",
                        tsar_void_ty>;
                        //[tsar_di_loc_ty, tsar_addr_ty, tsar_di_var_ty, 
                        //tsar_arr_base_ty]>;

def write_var_end : Intrinsic<"sapforWriteVarEnd",
                        tsar_void_ty>;
                        //[tsar_di_loc_ty, tsar_addr_ty, tsar_di_var_ty]>;

def write_arr_end : Intrinsic<"sapforWriteArrEnd",
                        tsar_void_ty>;
                        //[tsar_di_loc_ty, tsar_addr_ty, tsar_di_var_ty, 
                        //tsar_arr_base_ty]>;

def func_begin : Intrinsic<"sapforFuncBegin",
                        tsar_void_ty>;

def func_call_begin : Intrinsic<"sapforFuncCallBegin",
                        tsar_void_ty>;

def func_end : Intrinsic<"sapforFuncEnd",
                        tsar_void_ty>;

def func_call_end : Intrinsic<"sapforFuncCallEnd",
                        tsar_void_ty>;
