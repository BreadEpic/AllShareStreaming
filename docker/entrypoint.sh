#!/bin/sh
set -e

# Make sure the server folder exists
mkdir -p ${ALLSHARE_PATH}/server

# Run main application
exec ${ALLSHARE_PATH}/allshare "$@"