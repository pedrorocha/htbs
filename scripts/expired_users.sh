#!/bin/bash

# TODO
# This script _MUST_ list which users have they credits expired.
# Another script should kill them sessions.

# loading configuration
source mylan.conf


TABLE=expired_users
QUERY="SELECT host FROM $TABLE"

# query database
psql -d $DB_NAME -U $DB_USER -c "$QUERY" -q -t -A -F ' '

# TODO check the return value of psql
# if $? != 0 then ...
