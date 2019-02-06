package DotDiff;
use base qw(Exporter);
@EXPORT_OK = qw(dot_diff);

use Graph;
use Graph::Reader::Dot;
use Text::Diff;

our $dot_reader = Graph::Reader::Dot->new;

# my $diff_str = diff('file1', 'file2');
sub dot_diff
{
  my ($fname1, $fname2) = @_;
  my $str1 = m_get_graph_rep($fname1);
  my $str2 = m_get_graph_rep($fname2);
  diff(\$str1, \$str2)
}

sub m_get_graph_rep
{
  my $fname = shift;
  my $g = $dot_reader->read_graph($fname);
  my @els = m_get_graph_edge_labels($g);
  my @strs = sort map qq#"$_->[0]" - "$_->[1]"#, @els;
  join "\n", @strs
}

sub m_get_graph_vert2label_map
{
  my $g = shift;
  my %ret = map {
    my $label = $g->get_vertex_attribute($_, 'label');
    ($_ => m_exclude_llvm_dbg_numbers($label))
  } $g->vertices;
  %ret
}

sub m_get_graph_edge_labels
{
  my $g = shift;
  my %v2l = m_get_graph_vert2label_map($g);
  map [@v2l{@$_}], $g->edges
}

sub m_exclude_llvm_dbg_numbers
{
  my $str = shift;
  $str =~ s/(\!(sapfor\.)?dbg \!)\d+/$1/g;
  $str
}

1
