#!/bin/bash

# loading configuration
source mylan.conf

#for arg in $@; do
#	FIELDS="$FIELDS, $arg"
#done

TABLE=users

if [[ -z $1 ]]; then
	echo -e "usage: $0 \"fields\""
	echo -e "\tfields are: login, password, credits, logged, logged_at, ..."
	exit 1
else
	FIELDS=$1
fi

# query database
psql -d $DB_NAME -U $DB_USER -c "SELECT $FIELDS FROM $TABLE" -q -t -A -F ' '
