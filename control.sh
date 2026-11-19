#!/bin/sh
set -eu

HOME_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
R=${CHH_ROOT:-}
STATE="$HOME_DIR/state"
BASELINE="$STATE/mic-baseline"
TARGET_STATE="$STATE/mic-target"
EXPERIMENTS="$STATE/experiments"
HOOK="$R/mnt/SDCARD/.userdata/tg5040/.hooks/boot.d/90-choochoohacker.sh"
MARKER='# ChooChooHacker owned MIC1 hook v1'
TEMP_MARKER='# ChooChooHacker owned MIC1 temporary reset v1'
BOOST='ADCL Input MIC1 Boost Switch'
GAIN='MIC1 gain volume'
ADC='ADC volume'

error() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
read_file() { if [ -r "$1" ]; then tr '\000\n' '  ' < "$1"; else printf 'unavailable'; fi; }
model() {
    if [ -r "$R/usr/trimui/bin/MainUI" ]; then
        strings "$R/usr/trimui/bin/MainUI" |
            sed -n '/^Trimui Smart Pro$/p; /^Trimui Brick$/p; /^Trimui Brick Pro$/p' | head -n 1
    fi
}
mixer() { timeout 3 amixer -D hw:audiocodec "$@"; }
value() {
    result=$(mixer cget "name=$1") || return 1
    result=$(printf '%s\n' "$result" | sed -n 's/^ *: values=//p')
    [ -n "$result" ] || return 1
    printf '%s\n' "$result"
}
valid_values() {
    case "$b" in on|off|0|1) ;; *) return 1 ;; esac
    case "$g" in ''|*[!0-9]*) return 1 ;; esac
    [ "$g" -le 31 ] || return 1
    case "$a" in *,*) ;; *) return 1 ;; esac
    l=${a%,*}; r=${a#*,}
    case "$l:$r" in *[!0-9:]*|:*|*:) return 1 ;; esac
    [ "$l" -le 255 ] && [ "$r" -le 255 ]
}
current() {
    b=$(value "$BOOST") && g=$(value "$GAIN") && a=$(value "$ADC") && valid_values
}
persistent_hook() { [ -f "$HOOK" ] && grep -Fxq "$MARKER" "$HOOK"; }
temporary_hook() { [ -f "$HOOK" ] && grep -Fxq "$TEMP_MARKER" "$HOOK"; }
owned_hook() { persistent_hook || temporary_hook; }
supported() {
    [ "$(model)" = 'Trimui Smart Pro' ] || error 'MIC1 writes are only enabled for the Smart Pro test profile.'
    [ "$(cat "$R/proc/sys/kernel/osrelease")" = '4.9.191' ] ||
        error 'Untested kernel: diagnostic only.'
    grep -q 'allwinner,a133' "$R/sys/firmware/devicetree/base/compatible" ||
        error 'Untested SoC: diagnostic only.'
    [ -f "$R/mnt/SDCARD/.system/tg5040/bin/run_hooks.sh" ] ||
        error 'NextUI hook runner not found: diagnostic only.'
}
load_baseline() {
    [ -f "$BASELINE" ] || return 1
    { IFS= read -r b; IFS= read -r g; IFS= read -r a; } < "$BASELINE" || return 1
    valid_values
}
restore() {
    [ -f "$BASELINE" ] || { printf 'No saved MIC1 changes to restore.\n'; return 0; }
    load_baseline || { printf 'ERROR: Invalid baseline; retained for recovery.\n' >&2; return 1; }
    failed=0
    mixer cset "name=$ADC" "$a" || failed=1
    mixer cset "name=$GAIN" "$g" || failed=1
    mixer cset "name=$BOOST" "$b" || failed=1
    [ "$failed" = 0 ] || { printf 'ERROR: Restore incomplete; baseline retained. Retry restore.\n' >&2; return 1; }
    expected="$b/$g/$a"
    current && [ "$b/$g/$a" = "$expected" ] ||
        { printf 'ERROR: Restore readback differs; baseline retained.\n' >&2; return 1; }
    rm -f "$TARGET_STATE" || return 1
    rm "$BASELINE" || return 1
    printf 'Original MIC1 mixer values restored.\n'
}
apply_mic() {
    if [ "$1" = on ]; then
        mixer cset "name=$BOOST" on &&
            mixer cset "name=$GAIN" 31 &&
            mixer cset "name=$ADC" 160,160 &&
            current && { [ "$b" = on ] || [ "$b" = 1 ]; } &&
            [ "$g" = 31 ] && [ "$a" = 160,160 ]
    else
        mixer cset "name=$BOOST" off &&
            mixer cset "name=$GAIN" 0 &&
            current && { [ "$b" = off ] || [ "$b" = 0 ]; } && [ "$g" = 0 ]
    fi
}
desired_state() {
    if [ -f "$TARGET_STATE" ]; then
        target=$(cat "$TARGET_STATE") || return 1
        case "$target" in on|off) printf '%s\n' "$target" ;; *) error 'Invalid saved mic target; refusing changes.' ;; esac
    elif persistent_hook; then
        # Older persistent hooks always enabled MIC1.
        printf 'on\n'
    else
        current || error 'Cannot read the current mic state.'
        if { [ "$b" = on ] || [ "$b" = 1 ]; } && [ "$g" = 31 ] && [ "$a" = 160,160 ]; then
            printf 'on\n'
        else printf 'off\n'; fi
    fi
}
set_mic() {
    requested=$1
    case "$requested" in on|off) ;; *) error 'Invalid mic target.' ;; esac
    current || error 'Cannot read mixer values before changing the mic.'
    previous_boost=$b; previous_gain=$g; previous_adc=$a
    baseline_existed=no
    if [ ! -f "$BASELINE" ]; then
        printf '%s\n%s\n%s\n' "$b" "$g" "$a" > "$BASELINE.new" &&
            mv "$BASELINE.new" "$BASELINE" || error 'Cannot save original mixer values; refusing changes.'
    else
        load_baseline || error 'Existing baseline is invalid; refusing to overwrite it.'
        baseline_existed=yes
    fi
    if ! apply_mic "$requested"; then
        printf 'ERROR: MIC1 change failed; rolling back.\n' >&2
        rollback_mic || printf 'ERROR: Rollback failed; recovery snapshot retained.\n' >&2
        return 1
    fi
    if ! printf '%s\n' "$requested" > "$TARGET_STATE.new" || ! mv "$TARGET_STATE.new" "$TARGET_STATE"; then
        printf 'ERROR: Cannot save the mic target; rolling back.\n' >&2
        rollback_mic || printf 'ERROR: Rollback failed; recovery snapshot retained.\n' >&2
        return 1
    fi
    printf 'MIC1 route set to %s. Physical microphone is NOT verified.\n' "$requested"
}
rollback_mic() {
    if [ "$baseline_existed" = no ]; then restore; return; fi
    mixer cset "name=$ADC" "$previous_adc" &&
        mixer cset "name=$GAIN" "$previous_gain" &&
        mixer cset "name=$BOOST" "$previous_boost" &&
        current && [ "$b/$g/$a" = "$previous_boost/$previous_gain/$previous_adc" ] || return 1
    printf 'Previous mic setting restored; persistence and original baseline retained.\n'
}
install_hook() {
    hook_marker=$1
    hook_action=$2
    if [ -f "$HOOK" ] && grep -Fxq "$hook_marker" "$HOOK"; then return 0; fi
    mkdir -p "$(dirname "$HOOK")" || return 1
    escaped=$(printf '%s' "$HOME_DIR" | sed "s/'/'\\\\''/g")
    if ! {
        printf '#!/bin/sh\n%s\n' "$hook_marker"
        printf "CHH_HOME='%s'\n" "$escaped"
        printf 'exec "$CHH_HOME/control.sh" %s >> "$CHH_HOME/boot.log" 2>&1\n' "$hook_action"
    } > "$HOOK.new" || ! chmod +x "$HOOK.new" || ! mv "$HOOK.new" "$HOOK"; then
        rm -f "$HOOK.new"
        return 1
    fi
}
enable_with_hook() {
    if ! set_mic "$3"; then
        if [ ! -f "$BASELINE" ] && owned_hook; then rm "$HOOK"; fi
        return 1
    fi
    if ! install_hook "$1" "$2"; then
        printf 'ERROR: Cannot install boot hook; rolling back.\n' >&2
        if restore; then
            if owned_hook; then rm "$HOOK"; fi
        fi
        return 1
    fi
}
status() {
    printf 'MODEL=%s\n' "$(model)"
    printf 'KERNEL=%s\n' "$(read_file "$R/proc/sys/kernel/osrelease")"
    printf 'OS=%s\n' "$(read_file "$R/mnt/SDCARD/.system/version.txt")"
    printf 'SOC=%s\n' "$(read_file "$R/sys/firmware/devicetree/base/compatible")"
    printf 'BATTERY=%s\n' "$(read_file "$R/sys/class/power_supply/axp2202-battery/capacity")"
    printf 'POWER=%s\n' "$(read_file "$R/sys/class/power_supply/axp2202-battery/status")"
    printf 'TEMP_MC=%s\n' "$(read_file "$R/sys/class/thermal/thermal_zone0/temp")"
    printf 'CPU_KHZ=%s\n' "$(read_file "$R/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")"
    printf 'MEMORY=%s\n' "$(sed -n '/^MemTotal:/p; /^MemAvailable:/p' "$R/proc/meminfo" | tr '\n' ' ')"
    printf 'AUDIO=%s\n' "$(read_file "$R/proc/asound/cards")"
    if current; then
        printf 'BOOST=%s\nGAIN=%s\nADC=%s\nMIXER=readable\n' "$b" "$g" "$a"
        if { [ "$b" = on ] || [ "$b" = 1 ]; } && [ "$g" = 31 ] && [ "$a" = 160,160 ]; then
            printf 'MIC_STATE=on\n'
        elif { [ "$b" = off ] || [ "$b" = 0 ]; } && [ "$g" = 0 ]; then
            printf 'MIC_STATE=off\n'
        else printf 'MIC_STATE=inactive\n'; fi
    else
        printf 'MIXER=ERROR: mixer values unavailable\nMIC_STATE=unavailable\n'
    fi
    if [ -e "$HOOK" ]; then
        if persistent_hook; then printf 'PERSIST=enabled\n';
        elif temporary_hook; then printf 'PERSIST=disabled\n';
        else printf 'PERSIST=ERROR: hook path occupied\n'; fi
    else printf 'PERSIST=disabled\n'; fi
    if temporary_hook; then printf 'TEMPORARY=until-restart\n'; else printf 'TEMPORARY=none\n'; fi
    if [ -f "$BASELINE" ]; then printf 'BASELINE=saved\n'; else printf 'BASELINE=none\n'; fi
    if [ -d "$R/sys/class/udc" ]; then
        printf 'UDC=%s\n' "$(ls "$R/sys/class/udc" | tr '\n' ' ')"
    else printf 'UDC=unavailable\n'; fi
    printf 'USB_ROLE=%s\n' "$(read_file "$R/sys/devices/platform/soc/usbc0/otg_role")"
}

report_file() {
    printf '\n== %s ==\n' "$1"
    if [ -r "$2" ]; then
        head -c 8192 "$2" | sed '/^[[:space:]]*[Ss][Ee][Rr][Ii][Aa][Ll][[:space:]_:-]/d'
        printf '\n[Section limited to 8192 bytes.]\n'
    else printf 'Unavailable.\n'; fi
}
report_tool() {
    printf '\n=='
    printf ' %s' "$@"
    printf ' ==\n'
    if command -v "$1" >/dev/null 2>&1; then
        {
            if timeout 3 "$@" 2>&1; then printf '\n[Command completed.]\n'; else
                code=$?
                printf '\nERROR: Command exited with code %s.\n' "$code"
            fi
        } | head -c 32768
        printf '\n[Section limited to 32768 bytes.]\n'
    else printf 'Unavailable: command not installed.\n'; fi
}
diagnostics() {
    printf 'ChooChooHacker diagnostic v2\nRead-only; no audio captured.\n'
    report_tool date -u
    status
    report_tool uname -srm
    report_file 'Operating system' "$R/etc/os-release"
    report_file 'CPU' "$R/proc/cpuinfo"
    report_file 'Memory' "$R/proc/meminfo"
    report_file 'Kernel modules' "$R/proc/modules"
    report_file 'Kernel sound module parameters' "$R/sys/module/snd/parameters/slots"
    report_file 'ALSA version' "$R/proc/asound/version"
    report_file 'ALSA cards' "$R/proc/asound/cards"
    report_file 'ALSA PCM devices' "$R/proc/asound/pcm"
    report_file 'ALSA device nodes' "$R/proc/asound/devices"
    report_tool arecord -l
    report_tool aplay -l
    report_tool arecord --version
    report_tool amixer --version
    report_tool "$HOME_DIR/choochoohacker.aarch64" --runtime-info
    report_tool sha256sum "$HOME_DIR/choochoohacker.aarch64" "$HOME_DIR/control.sh"
    report_tool sh "$HOME_DIR/control.sh" inputs
    cards=0
    for id_file in "$R"/proc/asound/card[0-9]*/id; do
        [ -r "$id_file" ] || continue
        cards=$((cards + 1))
        [ "$cards" -le 8 ] || { printf 'Additional cards omitted (limit 8).\n'; break; }
        card_id=$(cat "$id_file") || { printf 'ERROR: Cannot read card ID.\n'; continue; }
        case "$card_id" in ''|*[!A-Za-z0-9_-]*) printf 'ERROR: Invalid card ID.\n'; continue ;; esac
        report_tool amixer -D "hw:$card_id" contents
        printf '\n== Card %s driver ==\n' "$card_id"
        card_directory=${id_file%/id}
        card_number=${card_directory##*card}
        readlink -f "$R/sys/class/sound/card$card_number/device/driver" 2>&1 || printf 'Unavailable.\n'
        for stream in "${id_file%/id}"/stream[0-9]*; do
            [ -r "$stream" ] || continue
            report_file 'USB audio formats' "$stream"
        done
        pcm_count=0
        for pcm_info in "${id_file%/id}"/pcm[0-9]*/info; do
            [ -r "$pcm_info" ] || continue
            pcm_count=$((pcm_count + 1))
            [ "$pcm_count" -le 16 ] || { printf 'Additional PCM devices omitted (limit 16).\n'; break; }
            report_file 'PCM description' "$pcm_info"
            report_file 'PCM current hardware parameters' "${pcm_info%/info}/sub0/hw_params"
            report_file 'PCM current software parameters' "${pcm_info%/info}/sub0/sw_params"
            report_file 'PCM stream status' "${pcm_info%/info}/sub0/status"
        done
        for midi in "${id_file%/id}"/midi[0-9]*; do
            [ -r "$midi" ] || continue
            report_file 'Raw MIDI device' "$midi"
        done
    done
    report_file 'MIDI sequencer clients' "$R/proc/asound/seq/clients"
    report_file 'MIDI sequencer queues' "$R/proc/asound/seq/queues"
    report_tool amidi -l
    report_tool aconnect -l
    report_file 'Last discovery' "$HOME_DIR/discovery.log"
    printf '\nSupported PCM formats require explicit Discovery; not opened by this report.\n'
    printf 'MIC_STATE is the known MIC1 route; inactive is not proof of a disconnected mic.\n'
    printf '\nMic state describes mixer routing, not proof of a physical microphone.\n'
    printf 'Audio, environment, network configuration and serial-number files are not exported.\n'
    printf 'Review device and MIDI client names before sharing this report.\n'
}
export_log() {
    directory="$HOME_DIR/diagnostics"
    mkdir -p "$directory" || error 'Cannot create diagnostic directory.'
    report="$directory/diagnostic-$(date +%Y%m%d-%H%M%S)-$$.txt"
    [ ! -e "$report" ] && [ ! -e "$report.new" ] || error 'Diagnostic path already exists.'
    if ! diagnostics > "$report.new" 2>&1 || ! mv "$report.new" "$report"; then
        rm -f "$report.new"
        error 'Cannot export diagnostics.'
    fi
    printf 'REPORT=%s\n' "$report"
}

capture_inputs() {
    input_count=0
    for input_info in "$R"/proc/asound/card[0-9]*/pcm[0-9]*c/info; do
        [ -r "$input_info" ] || continue
        input_card_dir=${input_info%/pcm*c/info}
        input_card=$(cat "$input_card_dir/id") || { printf 'ERROR: Cannot read capture card ID.\n' >&2; continue; }
        input_device=$(sed -n 's/^device: *//p' "$input_info")
        case "$input_card" in ''|*[!A-Za-z0-9_-]*) printf 'ERROR: Invalid capture card ID.\n' >&2; continue ;; esac
        case "$input_device" in ''|*[!0-9]*) printf 'ERROR: Invalid capture device number.\n' >&2; continue ;; esac
        input_count=$((input_count + 1))
        [ "$input_count" -le 8 ] || { printf 'Additional capture inputs omitted (limit 8).\n' >&2; break; }
        printf 'INPUT_%s=hw:CARD=%s,DEV=%s\n' "$input_count" "$input_card" "$input_device"
    done
}
discover() {
    printf 'Discovery: opening each input briefly at 48 kHz, S16 stereo.\n'
    printf 'No mixer changes. A working stream is not proof of a physical microphone.\n'
    found=0
    opened=0
    discovered=
    available=$(capture_inputs) || return 1
    while IFS= read -r entry; do
        [ -n "$entry" ] || continue
        pcm=${entry#*=}
        found=$((found + 1))
        printf '\n== %s ==\n' "$pcm"
        if command -v arecord >/dev/null 2>&1; then
            report_tool arecord -D "$pcm" --dump-hw-params -f S16_LE -c 2 -r 48000 -d 1 -t raw /dev/null
        fi
        if timeout 4 "$HOME_DIR/choochoohacker.aarch64" --probe-device "$pcm"; then
            opened=$((opened + 1))
            if [ -z "$discovered" ]; then discovered=$pcm; fi
        else
            probe_code=$?
            printf 'ERROR: Input probe failed (exit %s).\n' "$probe_code"
        fi
    done <<EOF
$available
EOF
    printf '\nInputs tried: %s; streams opened: %s.\n' "$found" "$opened"
    if [ -n "$discovered" ]; then
        printf 'DISCOVERED_INPUT=%s\n' "$discovered"
    else
        printf 'No capture stream opened. Nothing was changed.\n'
        return 1
    fi
}

experiment_read() {
    ex_card=$1
    ex_numid=$2
    case "$ex_card" in ''|*[!A-Za-z0-9_-]*) error 'Invalid mixer card ID.' ;; esac
    case "$ex_numid" in ''|*[!0-9]*) error 'Invalid mixer control number.' ;; esac
    [ "${#ex_numid}" -le 8 ] && [ "$ex_numid" -gt 0 ] || error 'Invalid mixer control number.'
    ex_node=
    for ex_id in "$R"/proc/asound/card[0-9]*/id; do
        [ -r "$ex_id" ] || continue
        if [ "$(cat "$ex_id")" = "$ex_card" ]; then
            ex_directory=${ex_id%/id}
            ex_number=${ex_directory##*card}
            ex_node="$R/dev/snd/controlC$ex_number"
            break
        fi
    done
    [ -n "$ex_node" ] && [ -e "$ex_node" ] || error 'Selected sound card is unavailable.'
    ex_boot=$(cat "$R/proc/sys/kernel/random/boot_id") || error 'Cannot identify this boot; refusing mixer experiments.'
    case "$ex_boot" in ''|*[!A-Fa-f0-9-]*) error 'Invalid boot identity; refusing mixer experiments.' ;; esac
    ex_identity="$ex_boot/$(stat -Lc '%d:%i' "$ex_node")" || error 'Cannot identify this sound card.'
    ex_data=$(timeout 3 amixer -D "hw:$ex_card" cget "numid=$ex_numid") ||
        error 'Cannot read the selected mixer control.'
    ex_name=$(printf '%s\n' "$ex_data" | sed -n "s/^numid=[0-9]*,iface=MIXER,name='\(.*\)'$/\1/p")
    printf '%s\n' "$ex_name" | grep -Eiq 'mic|capture|adc|input' || return 1
    if printf '%s\n' "$ex_name" | grep -Eiq 'speaker|output|playback|headphone|lineout|dac'; then return 1; fi
    ex_header=$(printf '%s\n' "$ex_data" | sed -n '/^ *; type=/p')
    printf '%s\n' "$ex_header" | grep -q 'access=rw' || return 1
    ex_type=$(printf '%s\n' "$ex_header" | sed -n 's/^ *; type=\([^,]*\).*/\1/p')
    ex_count=$(printf '%s\n' "$ex_header" | sed -n 's/.*values=\([0-9]*\).*/\1/p')
    case "$ex_count" in ''|*[!0-9]*) return 1 ;; esac
    [ "$ex_count" -ge 1 ] && [ "$ex_count" -le 8 ] || return 1
    case "$ex_type" in
        BOOLEAN) ex_min=0; ex_max=1 ;;
        INTEGER)
            ex_min=$(printf '%s\n' "$ex_header" | sed -n 's/.*min=\(-\{0,1\}[0-9]*\).*/\1/p')
            ex_max=$(printf '%s\n' "$ex_header" | sed -n 's/.*max=\(-\{0,1\}[0-9]*\).*/\1/p') ;;
        ENUMERATED)
            ex_min=0
            ex_items=$(printf '%s\n' "$ex_header" | sed -n 's/.*items=\([0-9]*\).*/\1/p')
            case "$ex_items" in ''|*[!0-9]*) return 1 ;; esac
            [ "${#ex_items}" -le 9 ] && [ "$ex_items" -gt 0 ] || return 1
            ex_max=$((ex_items - 1)) ;;
        *) return 1 ;;
    esac
    for ex_bound in "$ex_min" "$ex_max"; do
        ex_unsigned=${ex_bound#-}
        case "$ex_unsigned" in ''|*[!0-9]*) return 1 ;; esac
        [ "${#ex_unsigned}" -le 10 ] || return 1
    done
    [ "$ex_min" -ge -2147483648 ] && [ "$ex_max" -le 2147483647 ] && [ "$ex_min" -le "$ex_max" ] || return 1
    ex_values=$(printf '%s\n' "$ex_data" | sed -n 's/^ *: values=//p')
    experiment_values "$ex_values" || return 1
    ex_signature="$ex_type|$ex_min|$ex_max|$ex_count|$ex_name"
}
experiment_values() {
    ex_remaining=$1
    ex_seen=0
    while :; do
        ex_value=${ex_remaining%%,*}
        ex_seen=$((ex_seen + 1))
        [ "$ex_seen" -le "$ex_count" ] || return 1
        if [ "$ex_type" = BOOLEAN ]; then
            case "$ex_value" in on|off) ;; *) return 1 ;; esac
        else
            ex_unsigned=${ex_value#-}
            case "$ex_unsigned" in ''|*[!0-9]*) return 1 ;; esac
            [ "${#ex_unsigned}" -le 10 ] || return 1
            [ "$ex_value" -ge "$ex_min" ] && [ "$ex_value" -le "$ex_max" ] || return 1
        fi
        case "$ex_remaining" in *,*) ex_remaining=${ex_remaining#*,} ;; *) break ;; esac
    done
    [ "$ex_seen" = "$ex_count" ]
}
experiment_controls() {
    ex_index=0
    ex_cards=0
    for ex_id in "$R"/proc/asound/card[0-9]*/id; do
        [ -r "$ex_id" ] || continue
        ex_cards=$((ex_cards + 1))
        [ "$ex_cards" -le 8 ] || break
        ex_list_card=$(cat "$ex_id") || error 'Cannot read mixer card ID.'
        case "$ex_list_card" in ''|*[!A-Za-z0-9_-]*) error 'Invalid mixer card ID.' ;; esac
        ex_controls=$(timeout 3 amixer -D "hw:$ex_list_card" controls) || error 'Cannot enumerate mixer controls.'
        while IFS= read -r ex_control; do
            ex_list_numid=$(printf '%s\n' "$ex_control" | sed -n "s/^numid=\([0-9]*\),iface=MIXER,name='.*'$/\1/p")
            [ -n "$ex_list_numid" ] || continue
            ex_list_name=$(printf '%s\n' "$ex_control" | sed -n "s/^numid=[0-9]*,iface=MIXER,name='\(.*\)'$/\1/p")
            printf '%s\n' "$ex_list_name" | grep -Eiq 'mic|capture|adc|input' || continue
            if printf '%s\n' "$ex_list_name" | grep -Eiq 'speaker|output|playback|headphone|lineout|dac'; then continue; fi
            if experiment_read "$ex_list_card" "$ex_list_numid"; then
                ex_index=$((ex_index + 1))
                ex_label=$(printf '%s' "$ex_name" | tr '|\n' '  ')
                printf 'CONTROL_%s=%s|%s|%s|%s|%s|%s|%s|%s\n' "$ex_index" "$ex_card" "$ex_numid" "$ex_label" "$ex_type" "$ex_min" "$ex_max" "$ex_count" "$ex_values"
                [ "$ex_index" -lt 32 ] || return 0
            else printf 'Input control %s on %s is not safely editable.\n' "$ex_list_numid" "$ex_list_card" >&2; fi
        done <<EOF
$ex_controls
EOF
    done
}
experiment_restore_one() {
    ex_snapshot=$1
    ex_file=${ex_snapshot##*/}
    ex_restore_card=${ex_file%.*}
    ex_restore_numid=${ex_file##*.}
    experiment_read "$ex_restore_card" "$ex_restore_numid" || error 'Input control changed; snapshot retained.'
    { IFS= read -r ex_saved_identity; IFS= read -r ex_saved_signature; IFS= read -r ex_saved_values; } < "$ex_snapshot" ||
        error 'Invalid experiment snapshot; retained.'
    [ "$ex_identity" = "$ex_saved_identity" ] && [ "$ex_signature" = "$ex_saved_signature" ] ||
        error 'Card was reconnected or control identity changed; refusing automatic restore. Snapshot retained.'
    experiment_values "$ex_saved_values" || error 'Invalid experiment values; snapshot retained.'
    timeout 3 amixer -D "hw:$ex_card" cset "numid=$ex_numid" "$ex_saved_values" || error 'Experiment restore failed; snapshot retained.'
    experiment_read "$ex_restore_card" "$ex_restore_numid" && [ "$ex_values" = "$ex_saved_values" ] ||
        error 'Experiment restore readback differs; snapshot retained.'
    rm "$ex_snapshot" || error 'Cannot remove completed experiment snapshot.'
}
experiment_restore() {
    [ -d "$EXPERIMENTS" ] || { printf 'No mixer experiments to restore.\n'; return 0; }
    ex_failed=0
    for ex_snapshot in "$EXPERIMENTS"/*; do
        [ -f "$ex_snapshot" ] || continue
        if (experiment_restore_one "$ex_snapshot"); then :; else ex_failed=1; fi
    done
    [ "$ex_failed" = 0 ] || error 'Some experiment restores failed. Snapshots retained; inspect before retrying.'
    rmdir "$EXPERIMENTS" || error 'Experiment directory has unexpected files; inspect before retrying.'
    printf 'Original experiment controls restored.\n'
}
experiment_set() {
    experiment_read "$1" "$2" || error 'Only writable input controls are allowed.'
    experiment_values "$3" || error 'Invalid input value or channel count.'
    ex_requested=$3
    mkdir -p "$EXPERIMENTS"
    ex_snapshot="$EXPERIMENTS/$ex_card.$ex_numid"
    if [ -f "$ex_snapshot" ]; then
        { IFS= read -r ex_saved_identity; IFS= read -r ex_saved_signature; } < "$ex_snapshot" ||
            error 'Invalid existing snapshot.'
        [ "$ex_identity" = "$ex_saved_identity" ] && [ "$ex_signature" = "$ex_saved_signature" ] ||
            error 'Input identity changed; refusing changes.'
    else
        ex_snapshot_count=0
        for ex_existing in "$EXPERIMENTS"/*; do
            [ -f "$ex_existing" ] || continue
            ex_snapshot_count=$((ex_snapshot_count + 1))
        done
        [ "$ex_snapshot_count" -lt 32 ] || error 'Experiment limit reached (32); restore first.'
        printf '%s\n%s\n%s\n' "$ex_identity" "$ex_signature" "$ex_values" > "$ex_snapshot.new" &&
            mv "$ex_snapshot.new" "$ex_snapshot" || error 'Cannot snapshot input; refusing changes.'
    fi
    if timeout 3 amixer -D "hw:$ex_card" cset "numid=$ex_numid" "$ex_requested" &&
        experiment_read "$1" "$2" && [ "$ex_values" = "$ex_requested" ]; then
        printf 'Input changed temporarily. Restore before disconnecting or restarting.\n'
    else
        printf 'ERROR: Experiment failed; restoring its original control.\n' >&2
        experiment_restore_one "$ex_snapshot"
        return 1
    fi
}

ACTION=${1:-status}
if [ "$ACTION" = status ]; then status; exit 0; fi
if [ "$ACTION" = diagnostics ]; then diagnostics; exit 0; fi
if [ "$ACTION" = export-log ]; then export_log; exit 0; fi
if [ "$ACTION" = inputs ]; then capture_inputs; exit 0; fi
if [ "$ACTION" = controls ]; then experiment_controls; exit 0; fi
if [ "$ACTION" = discover ]; then
    mkdir -p "$STATE"
    mkdir "$STATE/lock" 2>/dev/null || error 'Another action is running (or stale state/lock needs inspection).'
    trap 'rm -f "$HOME_DIR/discovery.log.new"; rmdir "$STATE/lock"' EXIT
    trap 'exit 1' HUP INT TERM
    result=0
    discover > "$HOME_DIR/discovery.log.new" 2>&1 || result=$?
    cat "$HOME_DIR/discovery.log.new"
    mv "$HOME_DIR/discovery.log.new" "$HOME_DIR/discovery.log" || error 'Cannot save discovery results.'
    exit "$result"
fi
case "$ACTION" in mic-on|mic-off|restore|persist-on|persist-off|cleanup|boot|reset-temp|experiment-set|experiment-restore) ;; *) error 'Unknown action.' ;; esac
if [ "$ACTION" = cleanup ] && [ ! -f "$BASELINE" ] && [ ! -e "$HOOK" ] && [ ! -d "$EXPERIMENTS" ]; then
    printf 'No app-owned mixer changes to clean up.\n'
    exit 0
fi
mkdir -p "$STATE"
mkdir "$STATE/lock" 2>/dev/null || error 'Another action is running (or stale state/lock needs inspection).'
trap 'rmdir "$STATE/lock"' EXIT
trap 'exit 1' HUP INT TERM

if [ "$ACTION" = experiment-set ]; then
    [ "$#" = 4 ] || error 'Usage: experiment-set CARD NUMID VALUES'
    experiment_set "$2" "$3" "$4"
    exit
fi
if [ -d "$EXPERIMENTS" ]; then
    case "$ACTION" in
        boot|reset-temp) printf 'WARNING: Pending manual experiments are not restored by boot hooks; inspect state/experiments.\n' >&2 ;;
        *) experiment_restore ;;
    esac
fi
if [ "$ACTION" = experiment-restore ]; then exit 0; fi
if [ "$ACTION" != cleanup ] || [ -f "$BASELINE" ] || [ -e "$HOOK" ]; then supported; fi

case "$ACTION" in
    mic-on|mic-off)
        if [ -e "$HOOK" ]; then
            owned_hook || error 'Unknown hook; refusing changes.'
        fi
        target=${ACTION#mic-}
        if persistent_hook; then enable_with_hook "$MARKER" boot "$target"
        else enable_with_hook "$TEMP_MARKER" reset-temp "$target"; fi ;;
    restore)
        if [ -e "$HOOK" ]; then
            temporary_hook || error 'Disable persistence first.'
        fi
        restore
        if temporary_hook; then rm "$HOOK"; fi ;;
    cleanup)
        if persistent_hook; then
            printf 'Persistent MIC1 setting left unchanged by request.\n'
        elif temporary_hook; then
            printf 'Temporary MIC1 setting left unchanged until restart or Restore.\n'
        elif [ -e "$HOOK" ]; then error 'Unknown hook; refusing automatic cleanup.'
        else restore; fi ;;
    persist-on)
        if [ -e "$HOOK" ]; then
            owned_hook || error 'Hook path already occupied; not overwriting.'
        fi
        target=$(desired_state)
        enable_with_hook "$MARKER" boot "$target"
        printf 'NextUI boot hook installed. No system startup files changed.\n' ;;
    persist-off)
        if [ -e "$HOOK" ]; then
            owned_hook || error 'Not removing a hook owned by another tool.'
            rm "$HOOK"
        fi
        restore
        printf 'Persistence disabled.\n' ;;
    boot)
        persistent_hook || error 'Boot action requires our persistent hook.'
        load_baseline || error 'Boot baseline missing or invalid; refusing changes.'
        target=$(desired_state)
        set_mic "$target" ;;
    reset-temp)
        temporary_hook || error 'Temporary reset requires our reset hook.'
        restore
        rm "$HOOK"
        printf 'Temporary patch removed at restart.\n' ;;
esac
