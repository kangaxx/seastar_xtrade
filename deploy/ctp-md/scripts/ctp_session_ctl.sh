#!/usr/bin/env bash
# One entry point per systemd timer action: start-day, start-night, stop-day,
# stop-night, stop-late-night, health, calendar-check, status.
# Source: user daemonization request, 2026-11-25.
set -u

export TZ=Asia/Shanghai
export LC_ALL=C.UTF-8

CALENDAR=/usr/local/libexec/ctp-md/ctp_trading_calendar.py
ENV_FILE=/etc/xtrader/ctp-md.env
SERVICE=ctp-md.service
STATE_DIR=/var/lib/ctp-md
RESTART_LOG=$STATE_DIR/restart-log

: "${ENABLE_DAY:=1}"
: "${ENABLE_NIGHT:=1}"
: "${DAY_START_WINDOW:=08:30-13:00}"
: "${NIGHT_START_WINDOW:=20:30-22:30}"
: "${DAY_PRESENT_WINDOW:=08:30-15:20}"
: "${NIGHT_PRESENT_WINDOW:=20:30-23:05}"
: "${DAY_TICK_WINDOW:=09:00-11:30,13:30-15:20}"
: "${NIGHT_TICK_WINDOW:=21:00-23:05}"
: "${STALE_SECS_DAY:=600}"
: "${STALE_SECS_NIGHT:=600}"
: "${STATUS_MAX_AGE_SECS:=90}"
: "${START_GRACE_SECS:=600}"
: "${RESTART_LIMIT_PER_HOUR:=5}"
: "${STATUS_FILE:=/var/lib/xtrader/market/daemon.status}"

[ -f "$ENV_FILE" ] && . "$ENV_FILE"

log() { logger -t ctp-md-ctl "$*"; echo "[$(date '+%F %T')] $*"; }

in_window() { python3 "$CALENDAR" in-window --kind "$1" --window "$2" >/dev/null; }

service_active() { systemctl is-active --quiet "$SERVICE"; }

in_session() {
    if [ "$ENABLE_DAY" = 1 ] && in_window day "$DAY_PRESENT_WINDOW"; then return 0; fi
    if [ "$ENABLE_NIGHT" = 1 ] && in_window night "$NIGHT_PRESENT_WINDOW"; then return 0; fi
    return 1
}

start_service() {
    service_active && return 0
    if systemctl start "$SERVICE"; then
        log "started $SERVICE"
    else
        log "ERROR systemctl start failed ($(systemctl show -p ActiveState -p SubState --value "$SERVICE" 2>/dev/null | tr '\n' ' '))"
    fi
}

rate_limit_allows() {
    mkdir -p "$STATE_DIR"
    touch "$RESTART_LOG"
    local now; now=$(date +%s)
    awk -v now="$now" '$1 > now - 3600' "$RESTART_LOG" > "$RESTART_LOG.tmp"
    mv "$RESTART_LOG.tmp" "$RESTART_LOG"
    local count; count=$(wc -l < "$RESTART_LOG")
    [ "$count" -lt "$RESTART_LIMIT_PER_HOUR" ]
}

record_restart() { date +%s >> "$RESTART_LOG"; }

maybe_restart() {
    if ! rate_limit_allows; then
        log "CRITICAL restart limit reached; refusing further restarts this hour"
        return 0
    fi
    record_restart
    if systemctl restart "$SERVICE"; then
        log "restarted $SERVICE"
    else
        log "ERROR systemctl restart failed"
    fi
}

cmd_start_day() {
    [ "$ENABLE_DAY" = 1 ] || return 0
    if in_window day "$DAY_START_WINDOW"; then start_service; fi
}

cmd_start_night() {
    [ "$ENABLE_NIGHT" = 1 ] || return 0
    if in_window night "$NIGHT_START_WINDOW"; then start_service; fi
}

cmd_stop() {
    if service_active; then
        if systemctl stop "$SERVICE"; then
            log "stopped $SERVICE"
        else
            log "ERROR systemctl stop failed"
        fi
    fi
}

cmd_health() {
    if ! in_session; then return 0; fi
    if ! service_active; then
        if rate_limit_allows; then record_restart; start_service; fi
        return 0
    fi
    local now; now=$(date +%s)
    if [ ! -r "$STATUS_FILE" ]; then
        log "ERROR status file missing while service active; restarting"
        maybe_restart
        return 0
    fi
    local last_activity ready ticks last_tick started_ms
    last_activity=$(awk -F= '/^last_activity_ms=/{print $2}' "$STATUS_FILE")
    ready=$(awk -F= '/^ready=/{print $2}' "$STATUS_FILE")
    ticks=$(awk -F= '/^ticks_written=/{print $2}' "$STATUS_FILE")
    last_tick=$(awk -F= '/^last_tick_ms=/{print $2}' "$STATUS_FILE")
    started_ms=$(awk -F= '/^started_ms=/{print $2}' "$STATUS_FILE")
    : "${last_activity:=0}" "${ready:=0}" "${ticks:=0}" "${last_tick:=0}" "${started_ms:=0}"

    if [ $(( (now * 1000 - last_activity) / 1000 )) -gt "$STATUS_MAX_AGE_SECS" ]; then
        log "WARN daemon loop stalled (status age > ${STATUS_MAX_AGE_SECS}s); restarting"
        maybe_restart
        return 0
    fi
    if [ "$ready" != 1 ]; then
        if [ $(( now - started_ms / 1000 )) -gt "$START_GRACE_SECS" ]; then
            log "WARN not ready after $(( now - started_ms / 1000 ))s (login/subscribe failure); restarting"
            maybe_restart
        fi
        return 0
    fi
    if [ "$ticks" -gt 0 ] && [ "$last_tick" -gt 0 ]; then
        local threshold=0
        if [ "$ENABLE_DAY" = 1 ] && in_window day "$DAY_TICK_WINDOW"; then threshold=$STALE_SECS_DAY; fi
        if [ "$ENABLE_NIGHT" = 1 ] && in_window night "$NIGHT_TICK_WINDOW"; then threshold=$STALE_SECS_NIGHT; fi
        if [ "$threshold" -gt 0 ] && [ $(( (now * 1000 - last_tick) / 1000 )) -gt "$threshold" ]; then
            log "WARN no tick for $(( (now * 1000 - last_tick) / 1000 ))s inside a tick window; restarting"
            maybe_restart
        fi
    fi
    return 0
}

cmd_calendar_check() {
    python3 "$CALENDAR" check-coverage --warn-days 30
}

cmd_status() {
    systemctl status "$SERVICE" --no-pager -l || true
    echo "---- journal tail ----"
    journalctl -u "$SERVICE" -n 30 --no-pager
}

case "${1:-}" in
    start-day) cmd_start_day ;;
    start-night) cmd_start_night ;;
    stop-day|stop-night|stop-late-night) cmd_stop ;;
    health) cmd_health ;;
    calendar-check) cmd_calendar_check ;;
    status) cmd_status ;;
    *) echo "usage: $0 {start-day|start-night|stop-day|stop-night|stop-late-night|health|calendar-check|status}" >&2; exit 1 ;;
esac
