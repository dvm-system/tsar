//===---- DiagnosticKinds.td - Defines TSAR diagnostics ----*- tablegen -*-===//
//
//                     Traits Static Analyzer (SAPFOR)
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
// This file defines source-level diagnostics.
//
//===----------------------------------------------------------------------===//

// Define the diagnostic levels.
class DiagLevel;
def Ignored   : DiagLevel;
def Note      : DiagLevel;
def Remark    : DiagLevel;
def Warning   : DiagLevel;
def Error     : DiagLevel;
def Fatal     : DiagLevel;

// All diagnostics emitted by the compiler are an indirect subclass of this.
class Diagnostic<string text, DiagLevel DL> {
  string         Text = text;
  DiagLevel      Level = DL;
}

class Ignored<string str>   : Diagnostic<str, Ignored>;
class Note<string str>      : Diagnostic<str, Note>;
class Remark<string str>    : Diagnostic<str, Remark>;
class Warning<string str>   : Diagnostic<str, Warning>;
class Error<string str>     : Diagnostic<str, Error>;
class Fatal<string str>     : Diagnostic<str, Fatal>;

//Copy of some Clang diagnostics to use them with other frontends.
def err_fe_unable_to_open_output : Error<"unable to open output file '%0': '%1'">;
def err_unable_to_rename_temp: Error<"unable to rename temporary '%0' to output file '%1': '%2'">;

def err_import_named : Error<"unable to import declaration %0">;
def err_import : Error<"unable to import">;
def warn_import_typedef : Warning<"import of typedef declaration is partially supported">;
def warn_import_variable_array : Warning<"import of variable-length array is partially supported">;
def warn_import_case : Warning<"import of case statement is partially supported">;

def err_unknown_directive : Error<"namespace '%0' does not contain directive '%1'">;
def err_unknown_clause : Error<"directive '%0' does not contain clause '%1'">;

def err_backup_file : Error<"unable to backup file">;
def note_not_transform : Note<"transfomration was not performed">;
def err_transform_system : Note<"unable to transform system file">;
def warn_reformat : Warning<"unable to reformat file">;
def warn_transform_internal : Note<"unable to transform file">;

def warn_remove_directive_in_macro : Warning<"unable to remove directive in macro">;
def warn_remove_directive_in_include : Warning<"unable to remove directive in include">;
def warn_remove_directive : Warning<"unable to remove directive">;
def warn_unexpected_directive : Warning<"unexpected directive ignored">;
def note_inline_no_call : Note<"no call suitable for inline is found">;

def warn_inline_support_cpp : Warning<"inline expansion in C++ sources is not fully supported">;

def warn_remove_useless_variables : Warning<"unable to remove useless variables">;

def warn_disable_inline_no_body : Warning<"disable inline expansion for function without definition">;
def warn_disable_inline_in_ternary : Warning<"disable inline expansion in conditional operator">;
def warn_disable_inline_in_comma : Warning<"disable inline expansion in comma operator">;
def warn_disable_inline_in_loop_cond : Warning<"disable inline expansion in conditional expression of loop">;
def warn_disable_inline_in_for_inc : Warning<"disable inline expansion in the third section of for-loop">;
def warn_disable_inline_system : Warning<"disable inline expansion of non-user defined function">;
def warn_disable_inline_variadic : Warning<"disable inline expansion of variadic function">;
def warn_disable_inline_recursive : Warning<"disable inline expansion of recursive function">;
def warn_disable_inline_unreachable : Warning<"disable inline expansion of unreachable call">;
def warn_disable_inline_in_logic_rhs : Warning<"disable inline expansion in right hand side of logical operator">;
def warn_disable_inline_in_include : Warning<"disable inline expansion in header file">;

def warn_disable_inline : Warning<"disable inline expansion">;
def note_inline_macro_prevent : Note<"macro prevent inlining">;
def note_inline_unresolvable_extern_dep : Note<"unresolvable external dependence prevents inlining">;
def note_inline_static_extern_dep : Note<"refernece to static external dependence prevents inlining">;
def note_inline_hidden_extern_dep : Note<"hidden external dependence prevents inlining">;
def note_inline_different_num_params : Note<"number of formal parameters differs from a number of call arguments">;

def remark_inline : Remark<"inline expansion of function call">;
def remark_remove_unreachable : Remark<"remove unreachable code">;
def remark_remove_de_decl : Remark<"remove unused declaration">;

def note_decl_hide : Note<"declaration hides other declaration">;
def note_expanded_from_here : Note<"expanded from here">;
def note_source_range_not_single_file : Note<"bounds of source range located in different files">;
def note_end_location : Note<"end is here">;

def err_assert : Error<"assert violation">;
def note_assert_no_macro : Note<"macro found">;

def warn_rename_macro_prevent : Warning<"macro prevent renaming">;

def warn_propagate_macro_prevent : Warning<"macro prevent expression propagation">;
def warn_disable_propagate_in_include : Warning<"disable expression propagation in header file">;
def warn_disable_propagate : Warning<"disable expression propagation">;
def note_propagate_hidden_dep : Note<"hidden dependence prevents propagation">;
def note_propagate_recurrence : Note<"recurrence prevents propagation">;
def note_propagate_not_available : Note<"expression is not available at propagation point">;
def note_propagate_new_value : Note<"value may differs in definition and propagation points or may produce side effect">;

def warn_disable_de_parameter : Warning<"disable dead code elimination for function parameters">;
def warn_disable_de : Warning<"disable dead code elimination">;
def note_de_macro_prevent : Warning<"macro prevent dead code elimination">;
def note_de_side_effect_prevent : Warning<"side effect prevent dead code elimination">;
def note_de_multiple_prevent : Warning<"live declaration prevent dead code elimination">;

def err_apc_insert_dvm_directive : Error<"unable to insert DVMH directive '%0'">;
def note_apc_not_single_decl_stmt : Note<"declaration statement must containt a single declaration">;
def note_apc_insert_macro_prevent : Note<"macro prevent insertion of directive">;
def note_apc_insert_multiple_directives : Note<"other directive has been already inserted in this location">;
def note_apc_no_proper_definition : Note<"proper definition for '%0' not found">;
def note_apc_insert_include_prevent : Note<"unable to insert directive in include">;
def err_apc_insert_template : Error<"unable to create template definition for '%0'">;
def err_apc_not_distr_decl_directive : Error<"distribution of declaration depends on usage location">;

def note_decl_insert_macro_prevent : Note<"unable to create declaration '%0' in macro">;

def remark_parallel_loop : Remark<"parallel execution of loop is possible">;
def warn_parallel_loop : Warning<"unable to create parallel directive">;
def warn_parallel_not_canonical : Warning<"unable to create parallel directive for loop not in canonical form">;
def warn_parallel_no_induction : Warning<"unable to create parallel directive for loop without induction variable">;
def note_parallel_multiple_induction : Note<"loop has multiple inducition variables">;
def note_parallel_localize_private_unable : Note<"unable to localize privatizable variable">;
def note_parallel_localize_reduction_unable : Note<"unable to localize reduction variable">;
def note_parallel_localize_induction_unable : Note<"unable to localize induction variable">;
def note_parallel_localize_inout_unable : Note<"unable to localize inout variable">;
def note_parallel_localize_global_unable : Note<"unable to localize global variable">;
def note_parallel_reduction_unknown : Note<"unknown reduction operation prevents parallel execution">;
def note_parallel_variable_not_analyzed : Note<"cannot analyze variable">;
def note_parallel_across_direction_unknown : Note<"unable to implement pipeline execution for a loop with unknown step">;
def note_parallel_ordered_entry_unknown : Note<"unable to place 'ordered' directive in the loop with an unknown entry point">;
def note_parallel_across_tie_unable : Note<"unable to tie dependence with the parallel loop">;

def warn_region_add_loop_unable : Warning<"unable to mark loop for optimization">;
def warn_region_add_call_unable : Warning<"unable to mark function call for optimization">;
def warn_region_not_found : Warning<"optimization region with name '%0' not found">;

def warn_disable_replace_struct : Warning<"disable structure replacement">;
def warn_disable_replace_struct_no_param : Warning<"unable to replace any variable except argument">;
def warn_disable_replace_struct_no_pointer : Warning<"disable replacement of a non-pointer type">;
def warn_disable_replace_struct_no_struct : Warning<"disable replacement of a non-structure type">;
def warn_disable_replace_struct_system : Warning<"disable replacement of a non-user defined function">;

def note_replace_struct_arrow: Note<"not-arrow access prevent replacement">;
def note_replace_struct_decl: Note<"unable to build declaration of a record member">;
def note_replace_struct_macro_prevent : Note<"macro prevent replacement">;
def note_replace_struct_de_decl : Note<"unable to remove unused declaration">;
def note_replace_struct_decl_internal : Note<"unable to replace declaration">;

def remark_replace_struct: Remark<"structure replacement">;

def warn_replace_call_unable : Warning<"unable to replace call expression">;
def warn_replace_call_indirect_unable : Warning<"unable to replace indirect call expression">;
def note_replace_call_no_md : Note<"replacement metadata not found for function %0">;

def error_replace_md_missing : Error<"missing replacement metadata">;
def error_replace_md_target_param_expected : Error<"expected replacement for target parameter">;
def error_replace_md : Error<"invalid replacement metadata">;
def error_replace_md_param_twice : Error<"replacement metadata for parameter already set">;
def error_replace_md_type_incompatible : Error<"type of target %select{member|parameter}0 and replacement parameter are not compatible">;

def note_replace_md_no_param : Note<"missing parameter in replacement metadata">;
def note_replace_md_here : Note<"replacement metadata set here">;

def error_function_args_number : Error<"function does not take %0 %plural{1:argument|:arguments}0">;
def error_directive_clause_twice : Error<"directive '%0' cannot contain more than one '%1' clause">;
def error_clause_expect_function : Error<"expected function name after '%0' clause">;
def error_expect_function_param : Error<"expected function parameter name">;

def note_record_member_unknown: Error<"record has no member '%0'">;
def note_declared_here: Note<"declared here">;


// Fortran-specific diagnostics.
def warn_fortran_no_execution_part: Warning<"unable to locate execution-part statmenets">;
