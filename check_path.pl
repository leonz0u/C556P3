#!/usr/bin/perl

# Works with test9

use strict "vars";

my $n1 = 1;
my $n2 = 2;

my $wrong = 0;
my $right = 0;
while (<STDIN>) {

    chomp;

    if ($_ =~ /DATA/) {
	
	my @m = ($_ =~  /\((\d+),(\d+)\)/);

	if (@m == 2) {

	    print "DATA packet sent over link $m[0] -> $m[1]";
		    
	    if (($m[0] == $n1 && $m[1] == $n2) || ($m[0] == $n2 && $m[1] == $n1)) {
		print " FAILED";
		++$wrong;
	    } else {
		print " OK";
		++$right;
	    }
	    print "\n";
	}
    }    
}

my $total = $right + $wrong;

if ($total == 0) {
    print "WARNING: no data transmissions in the log file!\n";
    exit;
}
    
my $wfr = sprintf("%.2f", (100*($wrong / $total.0)));

print "\n$wrong transmissions over suboptimal link ($wfr%)\n";
