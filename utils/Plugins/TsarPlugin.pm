#===--- TestPlugin.pm ---------- PTS Test Plugin ----------------*- Perl -*-===#
#
#                        Traits Static Analyzer (SAPFOR)
#
#===------------------------------------------------------------------------===#
#
# This is a plugin for the PTS (Process Task Set) system. This plugin enables
# to run set of tests to check TSAR functionality.
#
# Two type of tests can be used:
# (1) 'init': these tests create new sample for a specified input,
# (2) 'check': these compare a sample with output of a tested program.
#
# Configuration files should have a structure wich is discussed bellow.
#
# For the both types of tests the following variable must be set:
#   name = private_1
#   plugin = TsarPlugin
#   action = init #type of the test
#
# (1) 'init' (action = init)
#   # A file with correct results, required
#   sample = private_1.c
#
#   # Additional files which can be checked using 'diff' tools, optional
#   # sample_diff =
#
#   # Prefix of a single line comments, optional, default:
#   # ';' is for .ll sample,
#   # '!' for .f, .for, .f90, .fdv sample,
#   # '//' for .c, .h, .cxx, .cpp, .cdv sample.
#   comment = //
#
#   # List of commands that should be executed.
#   # Separate command should be quoted '...'
#   # Additional variables can be used to simplify command.
#   # A prefix may be set to identify lines that should be checked.
#   # Default prefix is CHECK. Note, that this is an option for this plugin and
#   # it should be separated wit '|'.
#   options = "-print-only=da -print-filename"
#   run =
#     'tsar $sample $options -print-step=1'
#     'tsar $sample $options -print-step=2 | -check-prefix=CHECK-1'
#
# (2) 'check' uses 'init' as a base configuration (action = check)
#   # Path to configuration file for an appropriate initialization action.
#   base = path-to-init.conf
#
#  Note, it is possible to use a comand like '| -check-prefix=...' to discard
#  a specified prefix which became unsed.
#===------------------------------------------------------------------------===#

package Plugins::TsarPlugin;
use Plugins::Base v0.4.3;
use base qw(Plugins::Base);

use File::Path qw(remove_tree);
use File::Spec::Functions qw(catfile rel2abs splitdir);
use File::Copy qw(copy);
use File::chdir;
use File::Compare;
use Text::Diff;
use File::Temp qw(tempdir);
use File::Basename qw(fileparse);

use Exceptions;
use ConfigFile;
use Plugins::lib::DotDiff qw(dot_diff);

use strict;

=head1 DESCRIPTION

TSAR (Traits Static AnalyzeR) test plugin for PTS (Process Task Set)

=cut

use constant vs => exists $ENV{VISUALSTUDIOVERSION};

# Checks files and copy them to a template directory.
#
# Arguments: task directory, template directory, additional suffix for copy,
# flag which is specified whether it is necessary to 'die' in case of errors.
# Returns names of copies (it does not matter whether it was possible to copy).
sub copy_files {
  my $task_dir = shift @_;
  my $tmp_dir = shift @_;
  my $suffix_copy = shift @_;
  my $quiet = shift @_;
  my @suffixes = qw(.h .c .cpp .cxx .cdv .f .for .f90 .fdv .ll);
  my @copy_list;
  for (@_) {
    my $file = rel2abs(catfile($task_dir, $_));
    my ($name, $path, $suffix) = fileparse($file, @suffixes);
    unless ($suffix) {
      remove_tree $tmp_dir;
      die "fail: unsupported extension of $file: expected @suffixes\n";
    }
    my $copy_file = $suffix_copy ? "$name.$suffix_copy$suffix" : "$name$suffix";
    if (!copy($file, catfile($tmp_dir, $copy_file)) && !$quiet) {
      remove_tree $tmp_dir;
      die "fail: can not copy $file to a temporary directory\n";
    }
    push @copy_list, $copy_file;
  }
  return @copy_list;
}

sub error
{
  our $err_prefix;
  throw Exception => $err_prefix.join('', @_);
}

sub m_rmtree($)
{
  my $n = 32;
  my $err;
  while ((remove_tree($_[0], {error => \$err}), @$err != 0) && $n-- > 0) {
    dbg1 and dprint("m_rmtree: another try\n");
    select undef, undef, undef, 0.1;
  }
  @$err == 0
}

sub process {
  my ($class, $task, $db) = @_;
  my $ret = 1;

  local our $err_prefix = $task->name.': ';

  my %comments = (
    '.ll' => ';',
    '.f' => '!',
    '.for' => '!',
    '.f90' => '!',
    '.fdv' => '!',
    '.c' => '//',
    '.h' => '//',
    '.cxx' => '//',
    '.cpp' => '//',
    '.cdv' => '//',
  );

  my $action = $task->get_var('', 'action', 'check');
  grep $action eq $_, qw(check init) or error("'action' variable has wrong value");
  $task->reload_config(
    multiline => {'' => [qw(sample_diff run)]},
    required  => {'' => [qw(sample run tsar)]});
  my $sample = $task->get_var('', 'sample');
  my @sample_diff = $task->get_arr('', 'sample_diff', []);
  my $comment = $task->get_var('', 'comment', '');
  unless ($comment) {
    my ($sample_name, $sample_path, $sample_suffix) = fileparse($sample, keys %comments);
    error("prefix of a single line comment can not be inferred from an extension".
          " of the sample, try to set the 'comment' variable manually") unless $sample_suffix;
    $comment = $comments{$sample_suffix};
  }
  dbg1 and dprint("comment is set to '$comment'");
  my @run = $task->get_arr('', 'run');

  dbg1 and dprint("create temporary output directory");
  my $work_dir = tempdir($task->name.'_XXXX', DIR => $CWD, CLEANUP => 0);
  my $output_file = catfile($work_dir, 'output.log');

  dbg1 and dprint("verify prefixes which identify sample lines");
  my $check_prefixes = '';
  for (@run) {
    my $check_prefix = 'CHECK';
    my ($exec, $check_args) = $_ =~ m/^(.*?)(?:\|\s*(.*)\s*)?$/;
    if ($check_args) {
      (($check_prefix) = $check_args =~ m/^-check-prefix=(.*)$/) or
        error("unknown check option '$check_args'");
      ($check_prefix ne '') or
        error("empty check prefix is not allowed");
    }
    dbg1 and dprint("prefix '$check_prefix' is verified");
    $check_prefixes .= "$comment$check_prefix: |";
  }
  $check_prefixes =~ s/\|$//;

  dbg1 and dprint("create copy of a sample and discard sample lines with specified prefixes '$check_prefixes'");
  open(my $sf, '<', $sample) or error("unable to open sample file '$sample'");
  open(my $out, '>', $output_file) or error("unable to open output file '$output_file'");
  my $line_idx = 0;
  while (my $line = <$sf>) {
    chomp $line;
    unless ($line =~ m/^$check_prefixes/) {
      print $out "$line\n";
      ++$line_idx;
    }
  }
  close $sf;
  close $out;

  dbg1 and dprint("backup sample files and discard lines with specified prefixes '$check_prefixes' in the original files");
  my $sample_backup = catfile($work_dir, $sample);
  copy($sample, $sample_backup) or $action eq 'init' or error("unable to backup sample file '$sample'");
  for (@sample_diff) {
    copy($_, catfile($work_dir, $_)) or $action eq 'init' or error("unable to backup sample file '$_'");
  }
  copy($output_file, $sample) or error("unable to remove sample lines from '$sample'");

  RUN: for (@run) {
    my $check_prefix = 'CHECK';
    my ($exec, $check_args) = $_ =~ m/^\s*(.*?)\s*(?:\|\s*(.*)\s*)?$/;
    !$check_args or (($check_prefix) = $check_args =~ m/^-check-prefix=(.*)$/);
    dbg1 and dprint("check prefix is set to '$check_prefix'");
    if (!$exec) {
      dbg1 and dprint("ignore empty command");
      next RUN;
    }
    $exec .= ' 2>&1';
    dbg1 and dprint("run '$exec'");
    my $output = `$exec`;
    $? and $output = "Error while processing " . $task->name . ".\n" . $output;
    $output =~ s/\n$//;
    $output =~ s/\n/\n$comment$check_prefix: /g;
    my $curr_path = $CWD;
    $curr_path =~ s/\\/\\\\/g;
    $output =~ s/(:?$curr_path|\.)(:?\\|\/)//g;
    $output = "$comment$check_prefix: $output\n";
    dbg1 and dprint("write output to a file");
    open(my $out, '>>', $output_file) or error("unable to open output file '$output_file'");
    print $out $output;
    close $out;
    if ($action eq 'check') {
      dbg1 and dprint("start comparison for '$exec'");
      dbg1 and dprint("compare output with sample '$sample_backup'");
      my @output = split /\n/, $output;
      open(my $sf, '<', $sample_backup) or error("unable to open sample file '$sample_backup'");
      while (my $line = <$sf>) {
        chomp $line;
        if ($line =~ m/^$comment$check_prefix: /) {
          ++$line_idx;
          my $output_row = shift @output;
          if ($line ne $output_row) {
            close $sf;
            print_out(
              $task->name . ": output and sample are not equal at line $line_idx with prefix".
              " '$check_prefix' (see '$output_file' .vs '$sample')\n"
            );
            vs and print_out('  ', rel2abs($output_file), "($line_idx)\n");
            vs and print_out('  ', rel2abs($sample), "($line_idx)\n");
            vs and print_out("  Tools.DiffFiles ", rel2abs($sample), ' ', rel2abs($output_file), "\n");
            $ret = 0;
            next RUN;
          }
        }
      }
      close $sf;
      ++$line_idx;
      if (@output) {
        print_out(
          $task->name . ": output and sample are not equal at line $line_idx with prefix".
          " '$check_prefix' (see '$output_file' .vs '$sample')\n"
        );
        $ret = 0;
        next RUN;
      }
      for (@sample_diff) {
        my $backup = catfile($work_dir, $_);
        dbg1 and dprint("compare output with sample '$backup'");
        my ($name, $path, $dot) = fileparse($_, qw(.dot));
        unless (-e "$_") {
          print_out($task->name . ": '$_': output has not been created by '$exec'\n");
          $ret = 0;
        } elsif (my $diff = $dot ? dot_diff($_, $backup) : diff($_, $backup)) {
          print_out($task->name . ": '$_': output and sample are note equal for '$exec'\n");
          print_out($diff);
          $ret = 0;
        }
      }
      dbg1 and dprint("end comparison for '$exec'");
    }
  }

  if ($ret) {
    if ($action eq 'init') {
      my @no_changes;
      for (@sample_diff) {
        error("sample '$_' has not been created") unless (-e "$_");
        my $backup = catfile($work_dir, $_);
        push @no_changes, $_ if (-e $backup && compare($_, $backup) == 0);
      }
      if (-e "$sample_backup" && compare($output_file, $sample_backup) == 0) {
        copy($sample_backup, $sample) or error("unable to restore sample file '$sample' from '$sample_backup'");
        push @no_changes, $sample;
      } elsif (!copy($output_file, $sample)) {
        error("can not copy output '$output_file' to sample '$sample'");
      }
      if (@no_changes) {
        print_out($task->name . " - no changes in @no_changes\n");
      }
    } elsif ($action eq 'check') {
      copy($sample_backup, $sample) or error("unable to restore sample file '$sample'");
    } else {
      error("unknown action specified '$action'");
    }
    m_rmtree($work_dir);
  } else {
    error("unknown action specified '$action'") unless ($action eq 'check');
    dbg1 and dprint("restore sample files");
    copy($sample_backup, $sample) or error("unable to restore sample file '$sample'");
    for (@sample_diff) {
      copy(catfile($work_dir, $_), $_) or error("unable to restore sample file '$_'");
    }
  }
  $ret;
}
1;
