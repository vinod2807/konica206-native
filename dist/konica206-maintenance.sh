#!/bin/sh
# konica206-maintenance: nightly state backup + app log rotation.
# Installed as /etc/cron.daily/konica206-maintenance (needs a running cron;
# harmless if cron is absent). Safe to run by hand any time.
#
# - Keeps 7 daily copies of /var/lib/konica206-native.state under
#   /var/backups/konica206-native/.
# - Rotates /tmp/konica206-native.log when it exceeds 50 MiB, keeping 8
#   generations (copytruncate style: the app keeps its fd, nothing restarts).
# - Never touches printers, queues, services or the old konica206uri stack.

BACKUP_DIR=/var/backups/konica206-native
STATE=/var/lib/konica206-native.state
LOG=/tmp/konica206-native.log
KEEP_STATE=7
KEEP_LOG=8
LOG_MAX_BYTES=52428800

mkdir -p "$BACKUP_DIR" || exit 0

if [ -f "$STATE" ]; then
  cp -p "$STATE" "$BACKUP_DIR/state.$(date +%F)" 2>/dev/null
  # shellcheck disable=SC2012
  ls -t "$BACKUP_DIR"/state.* 2>/dev/null | tail -n +$((KEEP_STATE + 1)) | xargs -r rm -f
fi

if [ -f "$LOG" ]; then
  size=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
  if [ "$size" -ge "$LOG_MAX_BYTES" ]; then
    mv -f "$LOG.$(($KEEP_LOG - 1))" /dev/null 2>/dev/null
    i=$((KEEP_LOG - 1))
    while [ "$i" -ge 1 ]; do
      [ -f "$LOG.$i" ] && mv -f "$LOG.$i" "$LOG.$((i + 1))"
      i=$((i - 1))
    done
    cp -p "$LOG" "$LOG.1" && : > "$LOG"
  fi
fi

exit 0
