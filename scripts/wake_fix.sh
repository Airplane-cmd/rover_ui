#!/data/data/com.termux/files/usr/bin/bash
# wake_fix.sh — recovery daemon for vrshell UI after wake in Guardian sweet-spot.
#
# Background: when Guardian process is killed (sweet-spot: no boundary, walk freely),
# pressing Quest power button to sleep + waking breaks vrshell UI panels. This daemon
# detects wake events and re-runs the recovery chain automatically, keeping Guardian dead.
#
# Install on Quest:
#   adb push scripts/wake_fix.sh /data/local/tmp/wake_fix.sh
#   /data/local/tmp/quest_termux_run.sh "cp /data/local/tmp/wake_fix.sh ~/wake_fix.sh && chmod +x ~/wake_fix.sh"
#   Create ~/.termux/boot/wake-fix -> exec ~/wake_fix.sh (autostart via Termux:Boot).
#
# Requires: rooted Quest (Magisk), termux uid pre-granted in Magisk policy DB.
# No dependency on Termux:API app (uses kernel wake_lock via su).

LOG=~/wake.log
POLL=3
SETTLE=4
WAKELOCK_NAME=rover_ui_fix
LAST=""

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"; }

log "wake_fix starting (pid=$$)"

su -c "echo $WAKELOCK_NAME > /sys/power/wake_lock" 2>>"$LOG"
log "kernel wake lock acquired: $WAKELOCK_NAME"

cleanup() {
    su -c "echo $WAKELOCK_NAME > /sys/power/wake_unlock" 2>/dev/null
    log "wake lock released, exiting"
    exit 0
}
trap cleanup INT TERM

while true; do
    CUR=$(su -c 'dumpsys power 2>/dev/null | grep -m1 mWakefulness=' 2>>"$LOG" | sed 's/.*mWakefulness=//;s/[^A-Za-z].*//')
    if [ -n "$CUR" ] && [ "$CUR" != "$LAST" ]; then
        log "wakefulness: '$LAST' -> '$CUR'"
        if [ "$CUR" = "Awake" ] && [ -n "$LAST" ] && [ "$LAST" != "Awake" ]; then
            log "wake detected, waiting ${SETTLE}s then applying recovery"
            sleep "$SETTLE"
            su -c '
                pm enable com.oculus.guardian
                am force-stop com.oculus.vrshell
                sleep 1
                monkey -p com.oculus.vrshell 1
                sleep 4
                am force-stop com.oculus.guardian
            ' >>"$LOG" 2>&1
            log "recovery applied"
        fi
        LAST="$CUR"
    fi
    sleep "$POLL"
done
