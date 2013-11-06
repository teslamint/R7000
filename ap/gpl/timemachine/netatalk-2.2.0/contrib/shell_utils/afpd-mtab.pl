#!/usr/bin/perl
#
# $Id: afpd-mtab.pl.in,v 1.1 2002-01-17 05:59:25 srittau Exp $
#
# Create an afpd.mtab on standard output from the mtab-format file on standard
# input.
#
#	afpd-mtab.pl < /etc/mtab > /etc/afpd.mtab
#
#    Modification history:
#
# created.  -- rgr, 9-Apr-01.
#

print("# afpd.mtab, generated by afpd-mtab.pl on ",
      `date`);
while (<>) {
    ($device, $mount_point, $fstype) = split;
    next
	if $device eq 'none' || $mount_point eq '/boot';
    printf("%2d %-10s %s\n", ++$did_index, $device, $mount_point);
}
