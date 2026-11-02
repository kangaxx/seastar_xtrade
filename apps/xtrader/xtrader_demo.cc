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
#include <seastar/core/app-template.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/future.hh>
#include <seastar/xtrader/runtime_engine.hh>
#include <seastar/xtrader/version.hh>

#include <iostream>

using namespace seastar;

int main(int ac, char** av) {
    // 打印版本信息
    std::cout << "========================================" << std::endl;
    std::cout << "X-Trader Seastar Trading Engine v"
              << xtrader::version << std::endl;
    std::cout << "Build: " << xtrader::build_type
              << " | Git: " << xtrader::git_commit << std::endl;
    std::cout << "========================================" << std::endl;

    app_template app;

    return app.run(ac, av, [] {
        return do_with(xtrader::runtime_engine{}, [] (xtrader::runtime_engine& runtime) {
            const xtrader::domain::order_request open_buy {
                .strategy_id = "bootstrap",
                .instrument_id = "rb9999",
                .direction = xtrader::domain::side::buy,
                .offset = xtrader::domain::offset_flag::open,
                .hedge = xtrader::domain::hedge_flag::speculation,
                .volume = 1,
                .price = 3500.0,
            };

            return runtime.start().then([&runtime, open_buy] {
                return runtime.submit_order(open_buy);
            }).then([&runtime] (xtrader::domain::order_status status) {
                const auto snapshot = runtime.positions().snapshot();
                std::cout << "xtrader demo order status=" << static_cast<int>(status)
                    << ", long_today=" << snapshot.long_today
                    << ", short_today=" << snapshot.short_today << std::endl;
                return runtime.stop();
            }).then([] {
                return 0;
            });
        });
    });
}
