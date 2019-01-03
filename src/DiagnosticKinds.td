// Define the diagnostic levels.
class DiagLevel;
def Ignored    : DiagLevel;
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

def warn_remove_directive_in_macro : Warning<"unable to remove directive in macro">;
def warn_unexpected_directive : Warning<"unexpected directive ignored">;
def note_inline_no_call : Note<"no call suitable for inline is found">;

def warn_inline_support_cpp : Warning<"inline expansion in C++ sources is not fully supported">;

def warn_disable_inline_no_body : Warning<"disable inline expansion for function without definition">;
def warn_disable_inline_in_ternary : Warning<"disable inline expansion in conditional operator">;
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

def remark_inline : Remark<"inline expansion of function call">;
def remark_remove_unreachable : Remark<"remove unreachable code">;

def note_decl_hide : Note<"declaration hides other declaration">;
def note_expanded_from_here : Note<"expanded from here">;
def note_source_range_not_single_file : Note<"bounds of source range located in different files">;
def note_end_location : Note<"end is here">;

def err_assert : Error<"assert violation">;
def note_assert_no_macro : Note<"macro found">;

def warn_rename_macro_prevent : Warning<"macro prevent renaming">;
