int f()
{
#if __COUNTER__ > 1
    return 1;
#else
    return 2;
#endif
}
