/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
// code based on gcc - Copyright (C) 1999, 2000, 2001, 2002 Free Software Foundation, Inc.

#include <algorithm>

/* Heuristic to set a default for GGC_MIN_EXPAND.  */
int ggc_min_expand_heuristic(unsigned int mem_limit)
{
    double min_expand = mem_limit;

    /* The heuristic is a percentage equal to 30% + 70%*(RAM/1GB), yielding
       a lower bound of 30% and an upper bound of 100% (when RAM >= 1GB).  */
    min_expand /= 1024;
    min_expand *= 70;
    min_expand = std::min(min_expand, 70.);
    min_expand += 30;

    return int(min_expand);
}

/* Heuristic to set a default for GGC_MIN_HEAPSIZE.  */
unsigned int ggc_min_heapsize_heuristic(unsigned int mem_limit)
{
    /* The heuristic is RAM/8, with a lower bound of 4M and an upper
       bound of 128M (when RAM >= 1GB).  */
    mem_limit /= 8;
    mem_limit = std::max(mem_limit, 4U);
    mem_limit = std::min(mem_limit, 128U);

    return mem_limit * 1024;
}
