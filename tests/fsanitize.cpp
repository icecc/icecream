void test()
    {
    int* arr = new int[10];
    delete[] arr;
    int r = arr[ 0 ];
    (void)r;
    }

int main()
    {
    test();
    return 0;
    }
