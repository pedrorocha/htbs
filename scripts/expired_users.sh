#!/bin/bash

# TODO
# This script _MUST_ list which users have they credits expired.
# Therefore we must kill them session. HA HA HA!
# Another script should kill sessions.

# loading configuration
source mylan.conf


TABLE=expired_users
QUERY="SELECT login, host FROM $TABLE"

# query database
psql -d $DB_NAME -U $DB_USER -c "$QUERY" -q -t -A -F ' '
