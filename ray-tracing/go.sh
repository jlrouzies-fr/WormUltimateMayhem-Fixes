#!/bin/sh
# Restart the sandbox game and get into a match: a Quick Game (random level), or with
# LEVEL=NAME a Versus deathmatch on that landscape (see python rt.py levels). The sandbox runs
# at a very low resolution with a 30 fps cap so it leaves the GPU to whatever else is running.
# To change level without restarting: python rt.py level NAME.
cd "$(dirname "$0")"
py -3 sandbox.py stop >/dev/null 2>&1
py -3 sandbox.py start "$@" 2>&1 | grep -v "^started\|^window:\|^after taming"
sleep 9
py -3 rt.py pacing 1 ${FPSCAP:-30} >/dev/null
if [ -n "$LEVEL" ]; then
  py -3 rt.py key enter          # title screen -> main menu
  sleep 4
  py -3 rt.py level "$LEVEL"
else
  py -3 rt.py keys enter,enter,enter 3000
  sleep ${LOADWAIT:-16}
fi
py -3 rt.py status | head -1
