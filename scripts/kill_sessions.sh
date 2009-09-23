#!/bin/bash

# TODO
# This script must be called every minute by cron.
# It must kill sessions and warn users about remaining times.

# loading configuration
source mylan.conf

KILL_CMD="gnome-session-save --kill --silent"

for DEAD_SESSION in $(./expired_users.sh); do
	echo $DEAD_SESSION

	# We must only kill the user's session
	# GDM's PostSession script must update database
	ssh root@$DEAD_SESSION $KILL_CMD

	if test $? != 0; then
		# Here we should raise some kind of exception...
		echo "FUDEU: Não consegui conectar a $DEAD_SESSION"
	fi
done
