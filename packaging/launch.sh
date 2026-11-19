#!/bin/sh
set -u
PAK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
cd "$PAK_DIR" || exit 1
exec >> "$PAK_DIR/ChooChooHacker.log" 2>&1

if ! ./control.sh setup-default >> "$PAK_DIR/mic-default.log" 2>&1; then
    printf '%s\n' 'WARNING: Default microphone setup failed; see mic-default.log.'
fi

# Same physical Nintendo A/B mapping as ChooChooTracker's NextUI launcher.
export SDL_GAMECONTROLLERCONFIG="${SDL_GAMECONTROLLERCONFIG:+$SDL_GAMECONTROLLERCONFIG
}030003f05e0400008e02000014010000,TRIMUI Player1,a:b1,b:b0,x:b3,y:b2,back:b6,start:b7,guide:b8,leftshoulder:b4,rightshoulder:b5,leftstick:b9,rightstick:b10,lefttrigger:a2,righttrigger:a5,leftx:a0,lefty:a1,rightx:a3,righty:a4,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,platform:Linux,"

./choochoohacker.aarch64
result=$?
if ! ./control.sh cleanup; then
    printf '%s\n' 'ERROR: Mixer cleanup failed. Open the app and retry Restore.'
    exit 1
fi
exit "$result"
