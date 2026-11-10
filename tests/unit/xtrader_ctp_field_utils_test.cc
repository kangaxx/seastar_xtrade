/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 */
#include <seastar/testing/thread_test_case.hh>
#include <xtrader/ctp_field_utils.hh>

#include <stdexcept>
#include <string>

// B4: validate synthetic fixed-size SDK strings without a CTP connection.
// Source: user request, 2026-11-10. Signature: GitHub Copilot (Auto).
SEASTAR_THREAD_TEST_CASE(xtrader_ctp_fixed_fields_reject_truncation_and_unterminated_input) {
    char field[4]{};
    seastar::xtrader::ctp_detail::write_field(field, "abc");
    BOOST_CHECK_EQUAL(std::string(seastar::xtrader::ctp_detail::ctp_string(field)), "abc");
    BOOST_CHECK_THROW(seastar::xtrader::ctp_detail::write_field(field, "abcd"), std::invalid_argument);
    BOOST_CHECK_EQUAL(std::string(seastar::xtrader::ctp_detail::ctp_string(field)), "abc");
    const seastar::sstring embedded_nul("a\0b", 3);
    BOOST_CHECK_THROW(seastar::xtrader::ctp_detail::write_field(field, embedded_nul), std::invalid_argument);

    const char unterminated[3] = {'x', 'y', 'z'};
    BOOST_CHECK_THROW(static_cast<void>(seastar::xtrader::ctp_detail::ctp_string(unterminated)),
                      std::runtime_error);
}
