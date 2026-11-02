/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <seastar/testing/thread_test_case.hh>
#include <seastar/xtrader/historical_data_manager.hh>

SEASTAR_THREAD_TEST_CASE(xtrader_historical_manager_missing_root_warns) {
    seastar::xtrader::historical_data_manager manager("/tmp/nonexistent_xtrader_data_root_for_test");
    std::string warning;
    auto datasets = manager.scan_datasets(&warning);

    BOOST_REQUIRE(datasets.empty());
    BOOST_REQUIRE(!warning.empty());
}
