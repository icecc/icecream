// See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80369

#define MACRO if( arg != arg ) return 1;

int f( int arg )
    {
    MACRO
    return 2;
    }
