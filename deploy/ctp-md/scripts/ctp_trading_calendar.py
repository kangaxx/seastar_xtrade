#!/usr/bin/env python3
# Futures trading calendar for the ctp-md scheduler: trading-day and night-session
# predicates, session windows, coverage warnings. Offline by design (no network at
# 08:30 check time). Calendar data: deploy/ctp-md/etc/trading_calendar.json.
# Source: user daemonization request, 2026-11-25.
import argparse
import json
import os
import sys
from datetime import date, datetime, timedelta

EXIT_OK = 0
EXIT_NO = 1
EXIT_COVERAGE = 2
EXIT_USAGE = 3

CALENDAR_FILE = os.environ.get("CTP_MD_CALENDAR", "/etc/xtrader/trading_calendar.json")

# Night sessions belong to the next trading day and only exist when that day
# follows within the normal weekend gap (<= 3 days). Any holiday in between
# cancels the evening session; verified against every 2026 exchange notice.
NIGHT_GAP_LIMIT_DAYS = 3


class CoverageError(Exception):
    pass


def parse_date(text):
    return datetime.strptime(text, "%Y-%m-%d").date()


def date_range(start, end):
    current = parse_date(start)
    last = parse_date(end)
    while current <= last:
        yield current
        current += timedelta(days=1)


def now_local():
    # naive now() + astimezone() honors the TZ environment variable, which the
    # systemd units and ctl wrapper fix to Asia/Shanghai.
    return datetime.now().astimezone()


def minutes_of_day(clock):
    hour, minute = clock.split(":")
    return int(hour) * 60 + int(minute)


def window_ranges(text):
    # "08:30-15:20" or "09:00-11:30,13:30-15:20"; a range whose end is not after
    # its start crosses midnight (e.g. 20:30-02:35 belongs to the evening before).
    return [(minutes_of_day(start), minutes_of_day(end))
            for start, end in (part.split("-") for part in text.split(","))]


class TradingCalendar:
    def __init__(self, path):
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
        self.holidays = set()
        for holiday in data.get("holidays", []):
            self.holidays.update(date_range(holiday["start"], holiday["end"]))
        self.holidays.update(parse_date(day) for day in data.get("extra_holidays", []))
        self.extra_trading = {parse_date(day) for day in data.get("extra_trading_days", [])}
        self.coverage_until = parse_date(data["coverage_until"]) if data.get("coverage_until") else None
        self.coverage_start = parse_date(data["coverage_from"]) if data.get("coverage_from") else None

    def is_trading_day(self, day):
        if day in self.extra_trading:
            return True
        if day.weekday() >= 5:
            return False  # futures exchanges never trade on weekends, even 调休 workdays
        self.require_coverage(day)
        return day not in self.holidays

    def require_coverage(self, day):
        if self.coverage_until and day > self.coverage_until:
            raise CoverageError("calendar ends at %s (querying %s); update %s after the exchange publishes next year's schedule"
                                % (self.coverage_until, day, CALENDAR_FILE))
        if self.coverage_start and day < self.coverage_start:
            raise CoverageError("calendar starts at %s (querying %s); extend %s" % (self.coverage_start, day, CALENDAR_FILE))

    def next_trading_day(self, day):
        for _ in range(32):
            day += timedelta(days=1)
            if self.is_trading_day(day):
                return day
        raise CoverageError("no trading day within 32 days after %s" % day)

    def has_night_session(self, day):
        if not self.is_trading_day(day):
            return False
        return (self.next_trading_day(day) - day).days <= NIGHT_GAP_LIMIT_DAYS


def in_window(day, minutes, ranges, session_predicate):
    for start, end in ranges:
        if end > start:
            if start <= minutes < end:
                return session_predicate(day)
        else:  # crosses midnight
            if minutes >= start:
                return session_predicate(day)
            if minutes < end:
                return session_predicate(day - timedelta(days=1))
    return False


def load_calendar():
    return TradingCalendar(CALENDAR_FILE)


def cmd_is_trading_day(args):
    calendar = load_calendar()
    day = parse_date(args.date) if args.date else now_local().date()
    trading = calendar.is_trading_day(day)
    print("%s %s" % ("trading" if trading else "non-trading", day))
    return EXIT_OK if trading else EXIT_NO


def cmd_has_night_session(args):
    calendar = load_calendar()
    day = parse_date(args.date) if args.date else now_local().date()
    has_night = calendar.has_night_session(day)
    print("%s night session on %s evening" % ("has" if has_night else "no", day))
    return EXIT_OK if has_night else EXIT_NO


def cmd_next_trading_day(args):
    calendar = load_calendar()
    day = parse_date(args.date) if args.date else now_local().date()
    print(calendar.next_trading_day(day).isoformat())
    return EXIT_OK


def cmd_in_window(args):
    calendar = load_calendar()
    predicate = calendar.is_trading_day if args.kind == "day" else calendar.has_night_session
    now = now_local()
    inside = in_window(now.date(), now.hour * 60 + now.minute, window_ranges(args.window), predicate)
    print("%s window kind=%s ranges=%s now=%s %s" %
          ("in" if inside else "outside", args.kind, args.window,
           now.strftime("%Y-%m-%d %H:%M:%S %Z"), "(%s)" % now.tzname()))
    return EXIT_OK if inside else EXIT_NO


def cmd_check_coverage(args):
    calendar = load_calendar()
    if calendar.coverage_until is None:
        print("WARN calendar has no coverage_until", file=sys.stderr)
        return EXIT_COVERAGE
    days_left = (calendar.coverage_until - now_local().date()).days
    if days_left <= args.warn_days:
        print("WARN calendar coverage ends in %d days (%s); update after the exchange publishes next year's schedule"
              % (days_left, calendar.coverage_until), file=sys.stderr)
    else:
        print("OK calendar coverage until %s" % calendar.coverage_until)
    return EXIT_OK


def main(argv):
    parser = argparse.ArgumentParser(description="Futures trading calendar helpers for ctp-md")
    subparsers = parser.add_subparsers(dest="command", required=True)

    sub = subparsers.add_parser("is-trading-day", help="exit 0 when DATE is a trading day")
    sub.add_argument("date", nargs="?", help="YYYY-MM-DD, default today")

    sub = subparsers.add_parser("has-night-session", help="exit 0 when DATE evening has a night session")
    sub.add_argument("date", nargs="?", help="YYYY-MM-DD, default today")

    sub = subparsers.add_parser("next-trading-day", help="print the first trading day after DATE")
    sub.add_argument("date", nargs="?", help="YYYY-MM-DD, default today")

    sub = subparsers.add_parser("in-window", help="exit 0 when now is inside WINDOW of a day/night session")
    sub.add_argument("--kind", required=True, choices=["day", "night"])
    sub.add_argument("--window", required=True, help="e.g. 08:30-15:20 or 09:00-11:30,13:30-15:20")

    sub = subparsers.add_parser("check-coverage", help="warn when the calendar is about to expire")
    sub.add_argument("--warn-days", type=int, default=30)

    args = parser.parse_args(argv)
    try:
        handlers = {
            "is-trading-day": cmd_is_trading_day,
            "has-night-session": cmd_has_night_session,
            "next-trading-day": cmd_next_trading_day,
            "in-window": cmd_in_window,
            "check-coverage": cmd_check_coverage,
        }
        return handlers[args.command](args)
    except CoverageError as error:
        print("ERROR %s" % error, file=sys.stderr)
        return EXIT_COVERAGE
    except (OSError, ValueError, KeyError) as error:
        print("ERROR %s" % error, file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
