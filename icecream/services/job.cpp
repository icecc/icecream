#include "job.h"
#include "logging.h"
#include "exitcode.h"

using namespace std;

void appendList( CompileJob::ArgumentsList &list,
                        const CompileJob::ArgumentsList &toadd )
{
    for ( CompileJob::ArgumentsList::const_iterator it = toadd.begin();
          it != toadd.end(); ++it )
        list.push_back( *it );
}
