package Plugins::TsarEnv;
use strict;
use base qw(Plugins::Base);

sub on_prepare {
  my $class = shift;
  my $task = shift;
  my $pind = \shift;
  my $all_tasks = shift;
  my $task_list = shift;
  my $db = shift;

  my $tsar = $task->get_var('', 'tsar');
  return if !$tsar;

  my $platform = $task->get_var('', 'platform');
  my $clang= $task->get_var('', 'clang');
  my $include = $task->get_var('', 'include');
  my $dvm = $task->get_var('', 'dvm', '');

  for (my $i = $$pind + 1; $i < @$all_tasks; $i++) {
    my $t = $all_tasks->[$i];
    last if $t->plugin eq 'TsarEnv';
    next if $t->plugin ne 'TsarPlugin';
    my $new_id = $t->id.($t->id->args ? ',' : ':')."tsar=$tsar";
    $new_id .= ",platform=$platform" if $platform;
    $new_id .= ",clang=$clang" if $clang;
    $new_id .= ",include=$include" if $include;
    $new_id .= ",dvm=$dvm" if $dvm;
    $all_tasks->[$i] = $db->new_task($new_id);
  }
}

1;
