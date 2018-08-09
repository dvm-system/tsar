// Define the diagnostic levels.
class DiagLevel;
def Ignore    : DiagLevel;
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

class Ignore<string str>    : Diagnostic<str, Ignore>;
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

def warn_unexpected_directive : Warning<"unexpected directive ignored">;
def remark_inline_no_call : Remark<"no call suitable for inline is found">;

def warn_disable_inline_no_body : Warning<"disable inline expansion for function without definition">;
def warn_disable_inline_in_ternary : Warning<"disable inline expansion in conditional operator">;
def warn_disable_inline_in_loop_cond : Warning<"disable inline expansion in conditional expression of loop">;
def warn_disable_inline_in_for_inc : Warning<"disable inline expansion in the third section of for-loop">;

def warn_disable_inline : Warning<"disable inline expansion">;
def remark_inline_macro_prevent : Remark<"macro prevent inlining">;
