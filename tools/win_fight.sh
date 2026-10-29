#!/bin/bash
# Pelea CPU VS CPU en el PC: lanza out-win/hackversus.exe desde bash (VAR=valor opcionales) y la navega con
# tools/win_fight.ps1 -Attach. Con HV_PROF=<s> el perfil arranca solo cuando la pelea está en pantalla.
# uso: tools/win_fight.sh [VAR=valor ...]      -> out-win/run.log, out-win/run.err, out-win/shots/
cd "$(dirname "$0")/.."
W=$(pwd -W)
rm -f out-win/prof.go
env "$@" HV_PROF_GO="$W/out-win/prof.go" out-win/hackversus.exe "$W/game/EBOOT.ELF" "$W/disc" > out-win/run.log 2> out-win/run.err &
powershell -ExecutionPolicy Bypass -File "$W/tools/win_fight.ps1" -Attach -FightSecs ${FIGHT_SECS:-25}
wait
