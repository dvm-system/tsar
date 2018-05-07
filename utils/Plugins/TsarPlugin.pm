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
#   # Prefix of a single line comments, optional, default:
#   # ';' is for .ll sample,
#   # 'C' for .f, .for, .f90, .fdv sample,
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
use base qw(Plugins::Base);

use File::Path qw(remove_tree);
use File::Spec::Functions qw(catfile);
use File::Copy qw(copy);
use File::chdir;
use File::Compare;
use File::Temp qw(tempdir);
use File::Basename qw(fileparse);

use Exceptions;
use ConfigFile;

use strict;

=head1 DESCRIPTION

TSAR (Traits Static AnalyzeR) test plugin for PTS (Process Task Set)

=cut

sub process {
  my ($class, $task, $db) = @_;
  my $ret = 1;

  my %comments = (
    '.ll' => ';',
    '.f' => 'C',
    '.for' => 'C',
    '.f90' => 'C',
    '.fdv' => 'C',
    '.c' => '//',
    '.h' => '//',
    '.cxx' => '//',
    '.cpp' => '//',
    '.cdv' => '//',
  );

  my $action = $task->get_var('', 'action');
  my $base_task = $task;
  if ($action eq 'check') {
    my $base_task_id = $task->get_var('', 'base');
    $base_task = $db->get_task($base_task_id);
  }
  $base_task->reload_config(
    multiline => {'' => [qw(run)]},
    required  => {'' => [qw(sample run)]});
  my $sample = $base_task->conf->{''}{'sample'};
  my $comment = $base_task->conf->{''}{'comment'};
  unless ($comment) {
    my ($sample_name, $sample_path, $sample_suffix) = fileparse($sample, keys %comments);
    throw Exception=> $task->name . ": prefix of a single line comment can not be inferred from an extension of the sample, try to set the 'comment' variable manually" unless $sample_suffix;
    $comment = $comments{$sample_suffix};
  }
  $task->DEBUG("comment is set to '$comment'");
  my @run = @{$base_task->conf->{''}{'run'}};
  @run or throw Exception => $task->name. "unspecified run command";

  $task->DEBUG("create temporary output directory");
  my $work_dir = tempdir($task->name.'_XXXX', DIR => $CWD, CLEANUP => 0);
  my $output_file = catfile($work_dir, 'output.log');

  $task->DEBUG("verify prefixes which identify sample lines");
  my $check_prefixes = '';
  for (@run) {
    my $check_prefix = 'CHECK';
    $_ =~ s/\$(\w+)/my $v = $base_task->get_var('', $1); ref $v ? "@$v" : $v/ge;
    my ($exec, $check_args) = $_ =~ m/^(.+?)(?:\|\s*(.*)\s*)?$/;
    if ($check_args) {
      (($check_prefix) = $check_args =~ m/^-check-prefix=(.*)$/) or
        throw Exception => $task->name . ": unknown check option '$check_args'";
      ($check_prefix ne '') or
        throw Exception => $task->name . ": empty check prefix is not allowed";
    }
    $check_prefixes .= "$comment$check_prefix: |";
  }
  $check_prefixes =~ s/\|$//;

  $task->DEBUG("create copy of a sample and discard sample lines with specified prefixes '$check_prefixes'");
  open(my $sf, '<', $sample) or throw Exception => $task->name . ": unable to open sample file '$sample'";
  open(my $out, '>', $output_file) or throw Exception => $task->name . ": unable to open output file '$output_file'";
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

  RUN: for (@run) {
    my $check_prefix = 'CHECK';
    $_ =~ s/\$(\w+)/my $v = $base_task->get_var('', $1); ref $v ? "@$v" : $v/ge;
    my ($exec, $check_args) = $_ =~ m/^\s*(.*?)\s*(?:\|\s*(.*)\s*)?$/;
    !$check_args or (($check_prefix) = $check_args =~ m/^-check-prefix=(.*)$/);
    $task->DEBUG("check prefix is set to '$check_prefix'");
    if (!$exec) {
      $task->DEBUG("ignore empty command");
      next RUN;
    }
    $exec .= ' 2>&1';
    $task->DEBUG("run '$exec'");
    my $output = `$exec`;
    if ($?) {
      my $err_file = catfile($work_dir, 'err.log');
      open(my $err, '>', $err_file) or throw Exception => $task->name . ": unable to write TSAR executable errors to file";
      print $err $output;
      close $err;
      throw Exception => $task->name . ": TSAR executable error (see '$err_file')" if $?;
    }
    $output =~ s/\n$//;
    $output =~ s/\n/\n$comment$check_prefix: /g;
    $output = "$comment$check_prefix: $output\n";
    $task->DEBUG("write output to a file");
    open(my $out, '>>', $output_file) or throw Exception => $task->name . ": unable to open output file '$output_file'";
    print $out $output;
    close $out;
    if ($action eq 'check') {
      $task->DEBUG("compare output with sample '$sample'");
      my @output = split /\n/, $output;
      open(my $sf, '<', $sample) or throw Exception => $task->name . ": unable to open sample file '$sample'";
      while (my $line = <$sf>) {
        chomp $line;
        if ($line =~ m/^$comment$check_prefix: /) {
          ++$line_idx;
          my $output_row = shift @output;
          if ($line ne $output_row) {
            close $sf;
            print $task->name . ": output and sample are not equal at line $line_idx with prefix '$check_prefix' (see '$output_file')\n";
            $ret = 0;
            next RUN;
          }
        }
      }
      close $sf;
      ++$line_idx;
      if (@output) {
        print $task->name . ": output and sample are not equal at line $line_idx\n";
        $ret = 0;
        next RUN;
      }
    }
  }
  if ($ret) {
    if ($action eq 'init') {
      if (-e "$sample" && compare($output_file, $sample) == 0) {
        print $task->name . " - not changes in sample";
      } elsif (!copy($output_file, $sample)) {
        throw Exception => $task->name . ": can not copy output '$output_file' to sample '$sample'";
      } else {
        print $task->name;
      }
    } elsif ($action eq 'check') {
      print $task->name;
    } else {
      throw Exception => $task->name . ": unknown action specified '$action'";
    }
    remove_tree $work_dir;
    print " - ok\n";
  }
  $ret;
}
1;
