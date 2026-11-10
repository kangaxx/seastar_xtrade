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
#include <iostream>
#include <string_view>

#ifndef XTRADER_ENTRY_NAME
#error "XTRADER_ENTRY_NAME must identify the independently built entry point"
#endif

// B1: these separate binaries cannot load configuration or connect to a broker.
// Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
int main(int argc, char** argv) {
    if (argc == 2 && (std::string_view(argv[1]) == "--help" ||
                      std::string_view(argv[1]) == "-h")) {
        std::cout << XTRADER_ENTRY_NAME << " (NOT OPERATIONAL)\n"
                  << "Only --help is available. This entry does not load configuration,\n"
                  << "connect to a broker, or submit orders. Its backend is not implemented.\n";
        return 0;
    }

    std::cerr << "[ERROR] " << XTRADER_ENTRY_NAME
              << " is not operational: its backend and safety checks are not implemented.\n"
              << "No configuration was loaded and no connection or order was attempted.\n";
    return 2;
}
