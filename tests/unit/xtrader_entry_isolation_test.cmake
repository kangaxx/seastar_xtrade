# B1: disabled entries must reject configuration, mode changes, and live opt-in.
# Source: user request, 2026-11-09. Signature: GitHub Copilot (Auto).
function (expect_disabled executable)
  execute_process (
    COMMAND "${CMAKE_COMMAND}" -E env XTRADER_ENABLE_LIVE=1 "${executable}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
  if (NOT "${result}" STREQUAL "2" OR NOT error MATCHES "not operational")
    message (FATAL_ERROR "${executable} accepted a disabled invocation: ${result}\n${output}\n${error}")
  endif ()
endfunction ()

foreach (executable IN ITEMS "${REPLAY}" "${PAPER}" "${CTP_SIM}" "${LIVE}")
  execute_process (
    COMMAND "${executable}" --help
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
  if (NOT "${result}" STREQUAL "0" OR NOT output MATCHES "NOT OPERATIONAL")
    message (FATAL_ERROR "${executable} has an unexpected help result: ${result}\n${output}\n${error}")
  endif ()
  expect_disabled ("${executable}")
  expect_disabled ("${executable}" --mode live)
  expect_disabled ("${executable}" --config /nonexistent/live-config --enable-live)
  expect_disabled ("${executable}" --help --config /nonexistent/live-config)
endforeach ()

if (CMAKE_HOST_UNIX AND NOT CMAKE_HOST_APPLE)
  find_program (READELF_EXECUTABLE NAMES readelf)
  if (NOT READELF_EXECUTABLE)
    message (FATAL_ERROR "readelf is required to verify simulation entry dependencies")
  endif ()
  foreach (executable IN ITEMS "${REPLAY}" "${PAPER}")
    execute_process (
      COMMAND "${READELF_EXECUTABLE}" -d "${executable}"
      RESULT_VARIABLE result OUTPUT_VARIABLE dependencies ERROR_VARIABLE error)
    if (NOT "${result}" STREQUAL "0" OR dependencies MATCHES "libthosttraderapi")
      message (FATAL_ERROR "${executable} has an unsafe or unreadable dependency list: ${error}")
    endif ()
    if ("${executable}" STREQUAL "${REPLAY}" AND dependencies MATCHES "libthostmduserapi")
      message (FATAL_ERROR "The pure replay entry must not link the CTP market API")
    endif ()
  endforeach ()
endif ()

message (STATUS "All trading entries remain disabled before configuration or connection")
