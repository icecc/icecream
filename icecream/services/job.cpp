#include "job.h"
#include "logging.h"
#include "exitcode.h"

using namespace std;

void appendList( std::list<string> &list,
                 const std::list<string> &toadd )
{
    // Cannot splice since toadd is a reference-to-const
    list.insert( list.end(), toadd.begin(), toadd.end() );
}

list<string> CompileJob::flags( Argument_Type argumentType ) const
{
    list<string> args;
    for ( ArgumentsList::const_iterator it = m_flags.begin();
          it != m_flags.end(); ++it )
        if ( it->second == argumentType )
            args.push_back( it->first );
    return args;
}

list<string> CompileJob::localFlags() const
{
    return flags( Arg_Local );
}

list<string> CompileJob::remoteFlags() const
{
    return flags( Arg_Remote );
}

list<string> CompileJob::restFlags() const
{
    return flags( Arg_Rest );
}

list<string> CompileJob::allFlags() const
{
    list<string> args;
    for ( ArgumentsList::const_iterator it = m_flags.begin();
          it != m_flags.end(); ++it )
        args.push_back( it->first );
    return args;
}
