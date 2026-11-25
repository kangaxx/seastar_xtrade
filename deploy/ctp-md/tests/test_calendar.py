#!/usr/bin/env python3
# Regression test for the trading calendar against the official 2026 exchange
# holiday notice (证监办发〔2025〕130号). Run after every calendar edit:
#   python3 tests/test_calendar.py
# Source: user daemonization request, 2026-11-25.
import os
import sys
import unittest
from datetime import date

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
os.environ["CTP_MD_CALENDAR"] = os.path.join(ROOT, "etc", "trading_calendar.json")

from ctp_trading_calendar import (  # noqa: E402
    CoverageError, TradingCalendar, in_window, window_ranges,
)

CALENDAR = TradingCalendar(os.environ["CTP_MD_CALENDAR"])

D = date


class TradingDayTests(unittest.TestCase):
    def test_regular_trading_days(self):
        for day in (D(2026, 2, 13), D(2026, 4, 7), D(2026, 12, 31), D(2026, 11, 25)):
            self.assertTrue(CALENDAR.is_trading_day(day), day)

    def test_holidays(self):
        for day in (D(2026, 1, 1), D(2026, 1, 2), D(2026, 2, 16), D(2026, 2, 23),
                    D(2026, 4, 6), D(2026, 5, 1), D(2026, 5, 5), D(2026, 6, 19),
                    D(2026, 9, 25), D(2026, 10, 1), D(2026, 10, 7)):
            self.assertFalse(CALENDAR.is_trading_day(day), day)

    def test_weekends_never_trade_even_on_调休(self):
        # Official notice calls these 周末休市 despite being 调休 workdays.
        for day in (D(2026, 2, 14), D(2026, 2, 28), D(2026, 5, 9),
                    D(2026, 9, 20), D(2026, 10, 10)):
            self.assertFalse(CALENDAR.is_trading_day(day), day)

    def test_regular_weekend(self):
        self.assertFalse(CALENDAR.is_trading_day(D(2026, 3, 7)))   # Saturday
        self.assertFalse(CALENDAR.is_trading_day(D(2026, 3, 8)))   # Sunday

    def test_beyond_coverage_fails_closed(self):
        with self.assertRaises(CoverageError):
            CALENDAR.is_trading_day(D(2027, 1, 4))


class NightSessionTests(unittest.TestCase):
    def test_official_no_night_session_dates(self):
        # Every "当晚不进行夜盘交易" date from the 2026 notice.
        for day in (D(2026, 2, 13), D(2026, 4, 3), D(2026, 4, 30),
                    D(2026, 6, 18), D(2026, 9, 24), D(2026, 9, 30)):
            self.assertFalse(CALENDAR.has_night_session(day), day)

    def test_regular_night_sessions(self):
        for day in (D(2026, 1, 5), D(2026, 2, 6), D(2026, 4, 7), D(2026, 5, 8)):
            self.assertTrue(CALENDAR.has_night_session(day), day)

    def test_first_day_after_long_holiday_has_night(self):
        # 10-08 resumes trading AND resumes night trading that evening.
        self.assertTrue(CALENDAR.has_night_session(D(2026, 10, 8)))

    def test_coverage_boundary_night_fails_closed(self):
        # 12-31's own status is known, but its night depends on 2027.
        with self.assertRaises(CoverageError):
            CALENDAR.has_night_session(D(2026, 12, 31))


class WindowTests(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(window_ranges("08:30-15:20"), [(510, 920)])
        self.assertEqual(window_ranges("09:00-11:30,13:30-15:20"), [(540, 690), (810, 920)])
        self.assertEqual(window_ranges("20:30-02:35"), [(1230, 155)])

    def test_same_day_window(self):
        ranges = window_ranges("21:00-23:05")
        predicate = CALENDAR.has_night_session
        self.assertTrue(in_window(D(2026, 2, 6), 22 * 60, ranges, predicate))
        self.assertFalse(in_window(D(2026, 2, 6), 20 * 60, ranges, predicate))
        self.assertFalse(in_window(D(2026, 2, 13), 22 * 60, ranges, predicate))  # no night

    def test_cross_midnight_window(self):
        ranges = window_ranges("20:30-02:35")
        predicate = CALENDAR.has_night_session
        # Saturday 01:00 belongs to Friday's evening session.
        self.assertTrue(in_window(D(2026, 2, 7), 60, ranges, predicate))
        self.assertTrue(in_window(D(2026, 2, 6), 22 * 60, ranges, predicate))
        self.assertFalse(in_window(D(2026, 2, 7), 10 * 60, ranges, predicate))


if __name__ == "__main__":
    unittest.main(verbosity=2)
