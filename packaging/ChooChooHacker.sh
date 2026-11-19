#!/bin/bash
set -u
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d /opt/system/Tools/PortMaster ]; then
    controlfolder=/opt/system/Tools/PortMaster
elif [ -d /opt/tools/PortMaster ]; then
    controlfolder=/opt/tools/PortMaster
elif [ -d "$XDG_DATA_HOME/PortMaster" ]; then
    controlfolder="$XDG_DATA_HOME/PortMaster"
else
    controlfolder=/roms/ports/PortMaster
fi
source "$controlfolder/control.txt" || exit 1
[ ! -f "$controlfolder/mod_${CFW_NAME}.txt" ] || source "$controlfolder/mod_${CFW_NAME}.txt"
get_controls
cd "/$directory/ports/choochoohacker" || exit 1
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
pm_platform_helper "$PWD/choochoohacker.aarch64"
./choochoohacker.aarch64 >> ChooChooHacker.log 2>&1
result=$?
./control.sh cleanup >> ChooChooHacker.log 2>&1 || result=1
pm_finish
exit "$result"
