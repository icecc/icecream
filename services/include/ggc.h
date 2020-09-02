#ifndef _GGC_H_
#define _GGC_H_

int
ggc_min_expand_heuristic(unsigned int mem_limit);
unsigned int
ggc_min_heapsize_heuristic(unsigned int mem_limit);

#endif // _GGC_H_
