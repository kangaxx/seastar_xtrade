if (NOT DEFINED CLI OR NOT DEFINED FIXTURE_BASE)
  message (FATAL_ERROR "CLI and FIXTURE_BASE are required")
endif ()

string (RANDOM LENGTH 16 ALPHABET 0123456789abcdef fixture_suffix)
set (fixture "${FIXTURE_BASE}/xtrader_cli_config_test_${fixture_suffix}")
set (config_root "${fixture}/config")
file (MAKE_DIRECTORY
  "${config_root}/a_real"
  "${config_root}/b_simu"
  "${config_root}/c_unknown"
  "${config_root}/d_missing"
  "${config_root}/e_conflict"
  "${config_root}/f_encoded"
  "${fixture}/empty")

file (WRITE "${config_root}/a_real/xtrader.ini"
  "account_env = real\ninvestor_id=INVESTOR_SENTINEL\npassword=PASSWORD_SENTINEL\n")
file (WRITE "${config_root}/b_simu/xtrader.ini"
  "account_env=simu\nbroker_id=BROKER_SENTINEL\nauth_code=AUTH_SENTINEL\n")
file (WRITE "${config_root}/c_unknown/xtrader.ini" "account_env=unexpected\n")
file (WRITE "${config_root}/d_missing/xtrader.ini" "# account_env=simu\n")
file (WRITE "${config_root}/e_conflict/xtrader.ini" "account_env=real\naccount_env=simu\n")
file (WRITE "${config_root}/f_encoded/xtrader.ini" "account_env=ENCv1:ENCODED_SENTINEL\n")
file (WRITE "${fixture}/input.txt" "5\nq\n")
file (WRITE "${fixture}/not_a_directory" "fixture\n")

file (SHA256 "${config_root}/a_real/xtrader.ini" before_hash)
execute_process (
  COMMAND "${CMAKE_COMMAND}" -E env "XTRADER_CLI_CONFIG_ROOT=${config_root}"
    "${CLI}" --smp 1 --memory 512M --overprovisioned
  INPUT_FILE "${fixture}/input.txt"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if (NOT "${result}" STREQUAL "0")
  message (FATAL_ERROR "Config inventory failed: ${result}\n${output}\n${error}")
endif ()

foreach (expected IN ITEMS
    "[REAL] configuration #1"
    "[SIMU] configuration #2"
    "[UNKNOWN] configuration #3"
    "[UNKNOWN] configuration #4"
    "[UNKNOWN] configuration #5"
    "[UNKNOWN] configuration #6"
    "unverified; account details hidden"
    "Account configuration creation is disabled")
  string (FIND "${output}" "${expected}" found)
  if (found EQUAL -1)
    message (FATAL_ERROR "Missing expected inventory result: ${expected}\n${output}\n${error}")
  endif ()
endforeach ()

foreach (secret IN ITEMS INVESTOR_SENTINEL PASSWORD_SENTINEL BROKER_SENTINEL
                         AUTH_SENTINEL ENCODED_SENTINEL)
  string (FIND "${output}${error}" "${secret}" found)
  if (NOT found EQUAL -1)
    message (FATAL_ERROR "Account data was exposed in CLI output")
  endif ()
endforeach ()

file (SHA256 "${config_root}/a_real/xtrader.ini" after_hash)
if (NOT before_hash STREQUAL after_hash OR EXISTS "${config_root}/guangfa01"
    OR EXISTS "${config_root}/simu01")
  message (FATAL_ERROR "The configuration inventory modified or created account files")
endif ()

execute_process (
  COMMAND "${CMAKE_COMMAND}" -E env "XTRADER_CLI_CONFIG_ROOT=${fixture}/empty"
    "${CLI}" --smp 1 --memory 512M --overprovisioned
  INPUT_FILE "${fixture}/input.txt"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if (NOT "${result}" STREQUAL "0" OR NOT output MATCHES "no environments found"
    OR NOT output MATCHES "Account configuration creation is disabled")
  message (FATAL_ERROR "Empty configuration inventory failed: ${result}\n${output}\n${error}")
endif ()
file (GLOB empty_entries "${fixture}/empty/*")
if (empty_entries)
  message (FATAL_ERROR "The CLI created files in the empty configuration root")
endif ()

execute_process (
  COMMAND "${CMAKE_COMMAND}" -E env "XTRADER_CLI_CONFIG_ROOT=${fixture}/not_a_directory"
    "${CLI}" --smp 1 --memory 512M --overprovisioned
  INPUT_FILE "${fixture}/input.txt"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if (NOT "${result}" STREQUAL "0" OR NOT output MATCHES "Configuration inventory is incomplete"
    OR NOT error MATCHES "Configuration root is not an accessible directory"
    OR output MATCHES "no environments found")
  message (FATAL_ERROR "Invalid configuration root was not reported: ${result}\n${output}\n${error}")
endif ()

file (REMOVE_RECURSE "${fixture}")
message (STATUS "CLI configuration inventory is read-only and does not expose credentials")
