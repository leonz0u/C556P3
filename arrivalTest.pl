#!/usr/bin/perl
#this script will simply check if sent packets reach their destination
# Used for test1-8 and the parallel_ test

use Switch;


if(@ARGV!=2){ 
	print("You must specify two arguments!\n<simulator_config_file> <simulator_output_file>\n");
	die();
}

open(CONFIG_FILE, $ARGV[0]) or die "while opening config file\n$!\n";

$count=0;

while(<CONFIG_FILE>){
	chomp;
	if(/xmit/){
		@splitLine=split(/\s+/, $_);
		$tempHash{instant}=$splitLine[0];
		@splitEdge=split(/\W+/, $splitLine[2]);
		$tempHash{source}=$splitEdge[1];
		$tempHash{destination}=$splitEdge[2];
		$tempHash{seq}=$count;
		$tempHash{rec}=0;
		$count++;
		push(@events, {%tempHash});
	}
}

close(CONFIG_FILE);


open(SIMULATOR_OUTPUT_FILE, $ARGV[1]) or die "while opening output file\n$!\n";

$rec=0;

while(<SIMULATOR_OUTPUT_FILE>){
	if(/Event_Recv_Pkt_On_Node/ && /DATA/){
		@splitLine=split(/[^\d\.]+/, $_);

		for($i=0; $i<@events; $i++){			
			if($events[$i]{seq}==$splitLine[3] && $events[$i]{destination}==$splitLine[2]){
				#print "REC\n$_\n";
				$events[$i]{rec}=1;
				$rec++;
				last;
			}
			elsif($events[$splitLine[3]]{rec}==1){
				#print "ERR\n$_\n";
				$rep++;
				last;
			}
		}
		
		
	}

}

close(SIMULATOR_OUTPUT_FILE);

for($i=0; $i<@events; $i++){
	if($events[$i]{rec}==0){
		print("Packet $i from ".$events[$i]{source}." to ".$events[$i]{destination}." was lost.\n");
	}
	
}

print($rec*100/@events." percent arrived.\n".$rep*100/@events." percent were wrongly retransmitted after arrival at their destination.\n");
