#include <config.h>
#include "environment.h"
#include <logging.h>
#include <errno.h>
#include <dirent.h>

using namespace std;

static string read_fromFILE( FILE *f )
{
    string output;
    if ( !f ) {
        log_error() << "no pipe " << strerror( errno ) << endl;
        return output;
    }
    char buffer[100];
    while ( !feof( f ) ) {
        size_t bytes = fread( buffer, 1, 99, f );
        buffer[bytes] = 0;
        output += buffer;
    }
    pclose( f );
    return output;
}

static bool extract_version( string &version )
{
    string::size_type pos = version.find_last_of( '\n' );
    if ( pos == string::npos )
        return false;

    while ( pos + 1 == version.size() ) {
        version.resize( version.size() - 1 );
        pos = version.find_last_of( '\n' );
        if ( pos == string::npos )
            return false;
    }

    version = version.substr( pos + 1);
    return true;
}

list<string> available_environmnents()
{
    list<string> envs;
    string gcc_version = read_fromFILE( popen( "/usr/bin/gcc -v 2>&1", "r" ) );
    if ( extract_version( gcc_version ) )
        envs.push_back( "gcc " + gcc_version );
    string gpp_version = read_fromFILE( popen( "/usr/bin/g++ -v 2>&1", "r" ) );
    if ( extract_version( gpp_version ) )
        envs.push_back( "g++ " + gpp_version );

    string basedir = "/tmp/icecc-envs/";
    DIR *envdir = opendir( basedir.c_str() );
    if ( !envdir ) {
        log_info() << "can't open envs dir " << strerror( errno ) << endl;
    } else {
        struct dirent *ent = readdir(envdir);
        while ( ent ) {
            string dirname = ent->d_name;
            if ( dirname.at( 0 ) != '.' )
            {
                if ( !access( string( basedir + dirname + "/usr/bin/gcc" ).c_str(), X_OK ) )
                     envs.push_back( dirname );
            }
            ent = readdir( envdir );
        }
        closedir( envdir );
    }

    cout << "vers ";
    for ( list<string>::const_iterator it = envs.begin(); it != envs.end(); ++it )
        cout << "'" << *it << "' ";
    cout << endl;
    return envs;
}
