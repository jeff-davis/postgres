#----------------------------------------------------------------------
#
# Ranges.pm
#    Perl module for generating optimized C code that implements searching
#    value within ranges of values.
#
# This code generates a table with ranges of values and a function containing
# conditions for searching for a value in the table.
# This approach is very effective if there are consecutive values with gaps.
# The algorithm is well suited for creating effective character converters
# (case mapping, normalization, etc.).
#
# How it works.
#
# Values are sorted, after which ranges of values are created with a limit on
# the permissible gap between values.  If the gap between values is less than
# the limit, the range is considered continuous (not discontinuous), and the
# values in the gap are filled with zeros.  If the gap exceeds the specified
# limit, a new range is created.
#
# Let's take the following values with a gap limit of 10 as an example:
#     1, 2, 3, 50, 51, 1024, 1027, 1031
#
# We will obtain three ranges:
#     1. 1 to 3
#     2. 50 to 51
#     3. 1024 to 1031
#
# Next, a table is formed from these ranges, in which the data we need to
# compare with the values will be stored. In other words, an index.
#
# The values listed above will form the following table:
# {
#     0,                            /* The zero value is 0, it's like NULL */
#     'a', 'b', 'c',                /* 1 to 3 */
#     'j', 'k',                     /* 50 to 51 */
#     'x', 0, 0, 'y', 0, 0, 0, 'z'  /* 1024 to 1031 */
# }
#
# Next, the function code is generated to search for the required index in
# the table by value.
# For our example, it will look like this:
#
#     if (cp < 0x0034)
#     {
#         if (cp >= 0x0001 && cp < 0x0004)
#         {
#             return greek_table[cp - 0x0001 + 0];
#         }
#         else if (cp >= 0x0032)
#         {
#             return greek_table[cp - 0x0032 + 3];
#         }
#     }
#     else if (cp >= 0x0400)
#     {
#         if (cp < 0x0408)
#         {
#             return greek_table[cp - 0x0400 + 5];
#         }
#     }
#
#     return 0;
#
# In fact, we see that the code is a binary search built on if/else constructs
# that contain all the necessary data (checks and offsets) for correct indexing.
#
# Example of use:
#    # Grouping of code points with a gap of <= 10
#    my $ranges = Ranges::make([0x41..0x5A, 0x1F00, 0x1F08], 10);
#
#    # Table generation (gaps = 0)
#    print Ranges::tables($ranges, "greek_table", sub { $_[0] + 1 });
#
#    # Search code generation
#    print Ranges::branch_as_text($ranges, 0, $#$ranges, 0, "greek_table");
#
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/tools/Ranges.pm
#
#----------------------------------------------------------------------

package Ranges;

use strict;
use warnings FATAL => 'all';


# The function generates C code with a series of nested if-else conditions
# to search for the matching interval.
sub branch
{
	my ($range, $from, $to, $indent, $table_name) = @_;
	my ($idx, $space, $entry, @result);

	$idx = ($from + int(($to - $from) / 2));
	return \@result unless exists $range->[$idx];

	$space = "\t" x $indent;

	$entry = $range->[$idx];

	# IF state
	if ($idx == $from)
	{
		if ($idx == 0)
		{
			push @result,
			  sprintf("%sif (cp >= 0x%04X && cp < 0x%04X)\n%s{",
				$space, $entry->{Start}, $entry->{End}, $space);
		}
		else
		{
			push @result,
			  sprintf("%sif (cp < 0x%04X)\n%s{",
				$space, $entry->{End}, $space);
		}

		push @result,
		  sprintf("%s\treturn $table_name\[cp - 0x%04X + %d];",
			$space, $entry->{Start}, $entry->{Offset});
	}
	else
	{
		push @result,
		  sprintf("%sif (cp < 0x%04X)\n%s{", $space, $entry->{End}, $space);
		push @result,
		  @{ branch($range, $from, $idx - 1, $indent + 1, $table_name) };
	}

	push @result, $space . "}";

	# return now if it's the last range
	return \@result if $idx == (scalar @$range) - 1;

	# ELSE looks ahead to the next range to avoid adding an
	# unnecessary level of branching.
	$entry = @$range[ $idx + 1 ];

	# ELSE state
	push @result,
	  sprintf("%selse if (cp >= 0x%04X)\n%s{",
		$space, $entry->{Start}, $space);

	if ($idx == $to)
	{
		push @result,
		  sprintf("%s\treturn $table_name\[cp - 0x%04X + %d];",
			$space, $entry->{Start}, $entry->{Offset});
	}
	else
	{
		push @result,
		  @{ branch($range, $idx + 1, $to, $indent + 1, $table_name) };
	}

	push @result, $space . "}";

	return \@result;
}

sub branch_as_text
{
	return join "\n", @{ branch(@_) };
}

# Group numbers into ranges where the difference between neighboring
# elements does not exceed $limit. If the difference is greater, a new
# range is created. This is used to break the sequence into intervals
# where the gaps between numbers are greater than limit.
#
# For example, if there are numbers 1, 2, 3, 5, 6 and limit = 1, then
# there is a difference of 2 between 3 and 5, which is greater than 1,
# so there will be ranges 1-3 and 5-6.
sub make
{
	my ($nums, $limit) = @_;
	my ($prev, $start, $total, @sorted, @range);

	@sorted = sort { $a <=> $b } @$nums;

	die "expecting at least 2 codepoints" if (scalar @sorted < 2);

	$start = shift @sorted;
	$prev = $start;
	$total = 0;

	# append final 'undef' to signal final iteration
	push @sorted, undef;

	foreach my $curr (@sorted)
	{
		# if last iteration always append the range
		if (!defined($curr) || ($curr - $prev > $limit))
		{
			push @range,
			  {
				Start => $start,
				End => $prev + 1,
				Offset => $total
			  };
			$total += $prev + 1 - $start;
			$start = $curr;
		}

		$prev = $curr;
	}

	return \@range;
}

# The function combines all ranges into the $name table. Ranges may include
# entries without a mapping at all, in which the entry in the $name table
# should be zero.
# Arguments:
#	$range    - Reference to an array of ranges
#	$name     - Name of the generated C table
#	$callback - Function for calculating values
sub tables
{
	my ($range, $name, $callback) = @_;
	my @lines;

	foreach my $entry (@$range)
	{
		my $start = $entry->{Start};
		my $end = $entry->{End} - 1;

		foreach my $cp ($start .. $end)
		{
			my $idx = sprintf("%d,", $callback->($cp));
			$idx .= "\t" if length($idx) < 4;
			push @lines, sprintf("\t%s\t\t\t\t\t\t/* U+%06X */", $idx, $cp);
		}
	}

	my $length = scalar @lines;

	unshift @lines, "static const uint16 $name\[$length] =\n{";
	push @lines, "};";

	return join "\n", @lines;
}

1;
