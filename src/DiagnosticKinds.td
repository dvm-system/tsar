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
