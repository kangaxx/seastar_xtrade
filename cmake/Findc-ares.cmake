#
# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License").  See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership.  You may not use this file except in compliance with the License.
#
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

#
# Copyright (C) 2018 Scylladb, Ltd.
#

find_package (PkgConfig REQUIRED)

pkg_check_modules (PC_c-ares QUIET libcares)

find_library (c-ares_LIBRARY
  NAMES cares
  HINTS
    ${PC_c-ares_LIBDIR}
    ${PC_c-ares_LIBRARY_DIRS})

find_path (c-ares_INCLUDE_DIR
  NAMES ares_dns.h
  HINTS
    ${PC_c-ares_INCLUDEDIR}
    ${PC_c-ares_INCLUDE_DIRS})

if (c-ares_INCLUDE_DIR)
  foreach (v MAJOR MINOR PATCH)
    file(STRINGS "${c-ares_INCLUDE_DIR}/ares_version.h" ares_VERSION_LINE
      REGEX "^#define[ \t]+ARES_VERSION_${v}[ \t]+[0-9]+$")
    if (ares_VERSION_LINE MATCHES "ARES_VERSION_${v} ([0-9]+)")
      set (c-ares_VERSION_${v} "${CMAKE_MATCH_1}")
    endif ()
    unset (ares_VERSION_LINE)
  endforeach ()
  set (c-ares_VERSION ${c-ares_VERSION_MAJOR}.${c-ares_VERSION_MINOR}.${c-ares_VERSION_PATCH})
endif ()

mark_as_advanced (
  c-ares_LIBRARY
  c-ares_INCLUDE_DIR)

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (c-ares
  REQUIRED_VARS
    c-ares_LIBRARY
    c-ares_INCLUDE_DIR
  VERSION_VAR c-ares_VERSION)

if (c-ares_FOUND)
  set (c-ares_LIBRARIES ${c-ares_LIBRARY})
  set (c-ares_INCLUDE_DIRS ${c-ares_INCLUDE_DIR})
  if (NOT (TARGET c-ares::cares))
    add_library (c-ares::cares UNKNOWN IMPORTED)

    set_target_properties (c-ares::cares
      PROPERTIES
        IMPORTED_LOCATION ${c-ares_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${c-ares_INCLUDE_DIRS})
  endif ()

  include (CMakePushCheckState)
  include (CheckCXXSourceCompiles)
  cmake_push_check_state (RESET)
  set (CMAKE_REQUIRED_LIBRARIES c-ares::cares)
  unset (c-ares_HEADERS_MATCH_LIBRARY CACHE)
  check_cxx_source_compiles ("
    #include \"${c-ares_INCLUDE_DIR}/ares.h\"
    #if ARES_VERSION != ((${c-ares_VERSION_MAJOR} << 16) | (${c-ares_VERSION_MINOR} << 8) | ${c-ares_VERSION_PATCH})
    #error c-ares version header does not match the selected include directory
    #endif
    int main() {
      auto* volatile base = &ares_library_init;
      #if ARES_VERSION >= 0x011c00
      auto* volatile query = &ares_query_dnsrec;
      if (!query) return 1;
      #endif
      #if ARES_VERSION >= 0x012200
      auto* volatile socket_functions = &ares_set_socket_functions_ex;
      auto* volatile process = &ares_process_fds;
      if (!socket_functions || !process) return 1;
      #endif
      return !base;
    }" c-ares_HEADERS_MATCH_LIBRARY)
  cmake_pop_check_state ()
  if (NOT c-ares_HEADERS_MATCH_LIBRARY)
    message (FATAL_ERROR
      "The selected c-ares headers (${c-ares_INCLUDE_DIR}, version ${c-ares_VERSION}) "
      "and library (${c-ares_LIBRARY}) are incompatible. Check for conflicting "
      "header and library installations, then reconfigure with matching paths.")
  endif ()
endif ()
