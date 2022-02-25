/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Michael Matz <matz@suse.de>
                  2004 Stephan Kulow <coolo@suse.de>

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

#ifndef SCHEDULER_H
#define SCHEDULER_H

// Values 0 to 3.
#define DEBUG_SCHEDULER 0

// The weight the "fastest" scheduler places on using a
// server with recent statistics over one that has not
// been used for a while. Values 0 - 128, with higher
// values meaning the "fastest" servers are used more
// often.
#define STATS_UPDATE_WEIGHT 120

#include <iostream>
#include <string>

#include "../services/job.h"
#include "compileserver.h"


class SchedulerAlgorithmName {
public:
    enum Value: uint8_t {
        NONE=0,
        RANDOM,
        ROUND_ROBIN,
        LEAST_BUSY,
        FASTEST,
        UNDEFINED=255
    };

    SchedulerAlgorithmName() = default;
    constexpr SchedulerAlgorithmName(Value name)
        : name_{name}
    {}

    constexpr operator Value() const { return name_; }
    explicit operator bool() = delete;

    std::basic_string<char> to_string() const {
        switch (name_) {
            case SchedulerAlgorithmName::NONE:
                return "NONE";
            case SchedulerAlgorithmName::RANDOM:
                return "RANDOM";
            case SchedulerAlgorithmName::ROUND_ROBIN:
                return "ROUND_ROBIN";
            case SchedulerAlgorithmName::LEAST_BUSY:
                return "LEAST_BUSY";
            case SchedulerAlgorithmName::FASTEST:
                return "FASTEST";
            case SchedulerAlgorithmName::UNDEFINED:
                return "UNDEFINED";
        }
        return nullptr;
    }

private:
    Value name_;
};

inline std::basic_string<char> operator+=(std::basic_string<char> left, const SchedulerAlgorithmName &right) {
    return left.append(right.to_string());
}

inline std::ostream& operator<<(std::ostream &os, const SchedulerAlgorithmName &name) {
    os << name.to_string();
    return os;
}

#endif
