#!/usr/bin/env bash
# scoreboard.sh - live score + game status on the sign, from ESPN's public scoreboard feed.
#
#   scoreboard.sh nfl PHI        # Eagles
#   scoreboard.sh nhl PHI        # Flyers
#   scoreboard.sh nba PHI        # 76ers
#   scoreboard.sh mlb PHI        # Phillies
#
# Shows:   PHI:24         (team color)
#          Q3 5:12        (period/quarter + clock, kickoff time, or FINAL)
#          DAL:17
# Sends nothing when the team has no game in the current feed (the sign keeps what it had).
# Run it from cron every minute during games, e.g.:   * * * * 0  ~/matrix-sign/examples/scoreboard.sh nfl PHI
#
# Needs: curl, jq.  Uses ESPN's unofficial API, which can change without notice.
set -euo pipefail

LEAGUE=${1:-nfl}
TEAM=${2:-PHI}
COLOR=${3:-green}                       # your team's color on the sign
SIGN=${SIGN:-"$(dirname "$0")/../tools/sign.py"}

case "$LEAGUE" in
  nfl) PATHPART=football/nfl;   PERIOD=Q; REG=4 ;;
  nhl) PATHPART=hockey/nhl;     PERIOD=P; REG=3 ;;
  nba) PATHPART=basketball/nba; PERIOD=Q; REG=4 ;;
  mlb) PATHPART=baseball/mlb;   PERIOD=Inn; REG=99 ;;
  *) echo "league must be nfl, nhl, nba or mlb" >&2; exit 2 ;;
esac

msg=$(curl -fsS "https://site.api.espn.com/apis/site/v2/sports/$PATHPART/scoreboard" |
  jq -r --arg t "$TEAM" --arg c "$COLOR" --arg p "$PERIOD" --argjson reg "$REG" '
    .events[].competitions[0]
    | select(any(.competitors[]; .team.abbreviation == $t))
    | (.competitors[] | select(.team.abbreviation != $t) | .team.abbreviation) as $opp
    | (.competitors[] | select(.team.abbreviation == $t) | .score) as $us
    | (.competitors[] | select(.team.abbreviation != $t) | .score) as $them
    | .status as $s
    | (if   $s.type.state == "pre"  then ($s.type.shortDetail | split(" - ") | last | sub(" [A-Z]{2,4}$"; ""))
       elif $s.type.state == "post" then ($s.type.shortDetail | ascii_upcase)
       elif $s.displayClock == "0:00" then $s.type.shortDetail
       elif $s.period > $reg then "OT \($s.displayClock)"
       else "\($p)\($s.period) \($s.displayClock)" end) as $st
    | "{\($c)}\($t):\($us)\n{amber}\($st)\n{white}\($opp):\($them)"')

if [ -n "$msg" ]; then "$SIGN" text "$msg" --size 1 --static; fi
