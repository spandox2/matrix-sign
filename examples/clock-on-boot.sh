#!/usr/bin/env bash
# clock-on-boot.sh - put the sign back on its clock (e.g. from cron after other scripts finish).
#   */30 * * * *  ~/matrix-sign/examples/clock-on-boot.sh
SIGN=${SIGN:-"$(dirname "$0")/../tools/sign.py"}
exec "$SIGN" clock "$@"
