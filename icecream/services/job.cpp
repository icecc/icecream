#include "job.h"
#include "logging.h"
#include "exitcode.h"

using namespace std;

void appendList( std::list<string> &list,
                 const std::list<string> &toadd )
{
    for ( std::list<string>::const_iterator it = toadd.begin();
          it != toadd.end(); ++it )
        list.push_back( *it );
}

// TODO: write one template function. copy & paste sucks
list<string> CompileJob::localFlags() const
{
    list<string> args;
    for ( ArgumentsList::const_iterator it = m_flags.begin();
          it != m_flags.end(); ++it )
        if ( it->second == Arg_Local )
            args.push_back( it->first );
    return args;
}

list<string> CompileJob::remoteFlags() const
{
    list<string> args;
    for ( ArgumentsList::const_iterator it = m_flags.begin();
          it != m_flags.end(); ++it )
        if ( it->second == Arg_Remote )
            args.push_back( it->first );
    return args;
}

list<string> CompileJob::restFlags() const
{
    list<string> args;
    for ( ArgumentsList::const_iterator it = m_flags.begin();
          it != m_flags.end(); ++it )
        if ( it->second == Arg_Rest )
            args.push_back( it->first );
    return args;
}

list<string> CompileJob::allFlags() const
{
    list<string> args;
    for ( ArgumentsList::const_iterator it = m_flags.begin();
          it != m_flags.end(); ++it )
        args.push_back( it->first );
    return args;
}
