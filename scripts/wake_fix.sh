#!/data/data/com.termux/files/usr/bin/bash
# wake_fix.sh — recovery daemon for vrshell UI after headset mount in Guardian sweet-spot.
#
# Background: when Guardian process is killed (sweet-spot: no boundary, walk freely),
# taking the headset off + putting it back on breaks vrshell UI panels (passthrough only,
# no home/dash). This daemon reacts to Meta's proximity-mount logcat event and re-runs
# the recovery chain automatically, keeping Guardian dead.
#
# History:
#  v1: polled `dumpsys power | mWakefulness` every 3s. Missed proximity-sleep cycles
#      which flicker Asleep for <2s (headset off/on is often ~1s).
#  v2: rewritten to react to "[SEO] ShellApp: Proximity Sensor State Changed - mounted"
#      logcat event. Instant, never misses. Added DEBOUNCE_S because restarting vrshell
#      spawns a new ShellApp that re-emits a "mount" event on startup → recovery loop.
#
# Install on Quest:
#   adb push scripts/wake_fix.sh /data/local/tmp/wake_fix.sh
#   /data/local/tmp/quest_termux_run.sh "cp /data/local/tmp/wake_fix.sh ~/rover_ui_wake_fix.sh && chmod +x ~/rover_ui_wake_fix.sh"
#   ~/.termux/boot/rover-ui-wake-fix -> exec ~/rover_ui_wake_fix.sh (autostart via Termux:Boot).
#
# Requires: rooted Quest (Magisk), termux uid pre-granted in Magisk policy DB (needed
# for logcat main-buffer read + the recovery chain's pm/am/monkey calls).

LOG=~/rover_wake.log
SETTLE=4
DEBOUNCE_S=20
WAKELOCK_NAME=rover_ui_fix
TRIGGER='Proximity Sensor State Changed - mounted'

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"; }

log "rover_ui_wake_fix starting (pid=$$) — logcat-reactive mode, debounce=${DEBOUNCE_S}s"

su -c "echo $WAKELOCK_NAME > /sys/power/wake_lock" 2>>"$LOG"
log "kernel wake lock acquired: $WAKELOCK_NAME"

cleanup() {
    su -c "echo $WAKELOCK_NAME > /sys/power/wake_unlock" 2>/dev/null
    log "wake lock released, exiting"
    exit 0
}
trap cleanup INT TERM

# Reactive loop: watch logcat main buffer for proximity-mount, apply recovery on each match.
# Outer while re-launches the logcat pipe if it ever exits (kernel restart, buffer flush, etc).
# Debounce: recovery restarts vrshell → new ShellApp emits a startup mount → without debounce,
# every recovery re-triggers itself. Skip mount events within DEBOUNCE_S of last recovery.
LAST_RECOVERY=0
while true; do
    su -c 'logcat -T 1 -b main -v brief' 2>>"$LOG" \
      | grep --line-buffered -F "$TRIGGER" \
      | while read -r line; do
            NOW=$(date +%s)
            AGE=$(( NOW - LAST_RECOVERY ))
            if [ "$AGE" -lt "$DEBOUNCE_S" ]; then
                log "mount event ignored (debounce ${AGE}s < ${DEBOUNCE_S}s): $line"
                continue
            fi
            log "mount event: $line"
            log "settling ${SETTLE}s before recovery"
            sleep "$SETTLE"
            su -c '
                pm enable com.oculus.guardian
                am force-stop com.oculus.vrshell
                sleep 1
                monkey -p com.oculus.vrshell 1
                sleep 4
                am force-stop com.oculus.guardian
            ' >>"$LOG" 2>&1
            LAST_RECOVERY=$(date +%s)
            log "recovery applied"
        done
    log "logcat pipe closed — respawning in 5s"
    sleep 5
done
