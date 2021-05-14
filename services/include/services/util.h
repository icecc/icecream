/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef ICECREAM_UTIL_H
#define ICECREAM_UTIL_H

#include <string>
#include <sys/poll.h>
#include <vector>
#include <unistd.h>

extern std::string find_basename(const std::string &sfile);
extern std::string find_prefix(const std::string &basename);
// These two detect if the given binary is a C/C++ compiler based on its name(gcc->C,clang++->C++)
extern bool is_c_compiler(const std::string& compiler);
extern bool is_cpp_compiler(const std::string& compiler);
// These two return the given binary for the C or C++ compiler based on a compiler.
// E.g. get_c_compiler("clang++-8") -> "clang-8".
extern std::string get_c_compiler(const std::string& compiler);
extern std::string get_cpp_compiler(const std::string& compiler);

extern std::string read_command_output(const std::string& command,
    const std::vector<std::string>& args, int output_fd = STDOUT_FILENO);
// Returns just one line, with the trailing \n removed.
extern std::string read_command_line(const std::string& command,
    const std::vector<std::string>& args, int output_fd = STDOUT_FILENO);

template<typename T>
inline T ignore_result(T x __attribute__((unused)))
{
    return x;
}

// Returns true if _any_ of the given flags are set.
// If check_errors is set, then errors such as POLLHUP are also considered as matching.
bool pollfd_is_set(const std::vector<pollfd>& pollfds, int fd, int flags, bool check_errors = true);

std::string supported_features_to_string(unsigned int features);

#endif
