#!/usr/bin/env bash
# Idempotent installer for the ctp-md service + timers. Run as root from
# deploy/ctp-md/. The binary and encrypted config are installed separately
# (see README.md); this script only stages the scheduling layer.
set -eu

LIBEXEC=/usr/local/libexec/ctp-md
ETC=/etc/xtrader
SYSTEMD=/etc/systemd/system
RUN_USER=xtrader

cd "$(dirname "$0")"

install -d -m 0755 "$LIBEXEC" "$ETC" "$SYSTEMD"
install -m 0755 scripts/ctp_trading_calendar.py "$LIBEXEC/ctp_trading_calendar.py"
install -m 0755 scripts/ctp_session_ctl.sh "$LIBEXEC/ctp_session_ctl.sh"
install -m 0644 etc/trading_calendar.json "$ETC/trading_calendar.json"
[ -f "$ETC/ctp-md.env" ] || install -m 0644 etc/ctp-md.env "$ETC/ctp-md.env"
install -m 0644 systemd/*.service systemd/*.timer "$SYSTEMD/"

if ! id -u "$RUN_USER" >/dev/null 2>&1; then
    useradd --system --home-dir /var/lib/xtrader --shell /usr/sbin/nologin "$RUN_USER"
fi
install -d -m 0750 -o "$RUN_USER" -g "$RUN_USER" /var/lib/xtrader /var/log/xtrader
install -d -m 0755 -o "$RUN_USER" -g "$RUN_USER" /run/xtrader
install -d -m 0755 /var/lib/ctp-md

systemctl daemon-reload
systemctl enable --now ctp-md-health.timer
systemctl enable ctp-md-start-day.timer ctp-md-start-night.timer \
    ctp-md-stop-day.timer ctp-md-stop-night.timer ctp-md-calendar-check.timer

cat <<EOF
ctp-md scheduler installed.
- health timer armed: ctp-md-health.timer
- start/stop/calendar timers enabled (service NOT enabled: schedulers own start)
- optional for SHFE metals night sessions: systemctl enable ctp-md-stop-night-late.timer
Next steps (README.md):
  1. install binary + encrypted config (market.enc / market.key)
  2. grant write on /data/x_trader_data to user $RUN_USER
  3. smoke test: systemctl start ctp-md.service && journalctl -u ctp-md -f
EOF
