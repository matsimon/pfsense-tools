#!/bin/sh
#
# pfSense snapshot building system
# (C)2007, 2008, 2009 Scott Ullrich
# All rights reserved
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#

PWD=`pwd`

if [ ! -f "$PWD/pfsense-build.sh" ]; then
	echo "You must run this utility from the same location as pfsense-build.sh !!"
	exit 1
fi

# Handle command line arguments
while test "$1" != "" ; do
	case $1 in
		--noports|-n)
		echo "$2"
		NO_PORTS=yo
		shift
	;;
	esac
	shift
done

# Main builder loop
COUNTER=0
while [ /bin/true ]; do
	COUNTER=`expr $COUNTER + 1`
	echo ">>> Starting builder run #${COUNTER}..."
	echo
	# We can disable ports builds
	if [ "$NO_PORTS" != "" ]; then
		echo ">>> Not building pfPorts at all during this snapshot builder looped run..."
		touch /tmp/pfSense_do_not_build_pfPorts
	else
		if [ "$COUNTER" -gt 0 ]; then 
			echo ">>> Previous snapshot runs deteceted, not building pfPorts again..."
			touch /tmp/pfSense_do_not_build_pfPorts
		else
			rm -f /tmp/pfSense_do_not_build_pfPorts
		fi
	fi
	NANO_SIZE=`cat $PWD/pfsense-build.sh | grep FLASH_SIZE | cut -d'"' -f2`
	# Loop through each builder run and alternate between image sizes.
	# 512m becomes 1g, 1g becomes 2g, 2g becomes 4g, 4g becomes 512m.
	NEW_NANO_SIZE="512m"
	case $NANO_SIZE in
		"512m")
			NEW_NANO_SIZE="1g"
		;;
		"1g")
			NEW_NANO_SIZE="2g"	
		;;
		"2g")
			NEW_NANO_SIZE="4g"	
		;;
		"4g")
			NEW_NANO_SIZE="512m"	
		;;
	esac
	echo $NEW_NANO_SIZE > /tmp/nanosize.txt
	cat $PWD/pfsense-build.sh | grep -v FLASH_SIZE > /tmp/pfsense-build.sh
	echo "export FLASH_SIZE=\"${NEW_NANO_SIZE}\"" >>/tmp/pfsense-build.sh
	mv /tmp/pfsense-build.sh $PWD/pfsense-build.sh
	echo ">>> Previous NanoBSD size: $NANO_SIZE ... New size has been set to: $NEW_NANO_SIZE"
	sh ./build_snapshots.sh
	# Grab a random value and sleep
	value=`od -A n -d -N1 /dev/random | awk '{ print $1 }'`
	# Sleep for that time.
	echo
	echo ">>> Sleeping for $value in between snapshot builder runs"
	echo
	# Count some sheep.
	sleep $value
done
