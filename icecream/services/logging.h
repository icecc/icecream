#ifndef _LOGGING_H
#define _LOGGING_H

#include <iostream>

static inline std::ostream& log_info() {
    return std::cerr;
}

static inline std::ostream& log_crit() {
    return std::cerr;
}

static inline std::ostream& log_warning() {
    return std::cerr;
}

static inline std::ostream& log_error() {
    return std::cerr;
}

static inline std::ostream& trace() {
    return std::cerr;
}

#endif
