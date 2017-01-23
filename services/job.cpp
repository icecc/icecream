/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "job.h"
#include "logging.h"
#include "exitcode.h"
#include "platform.h"
#include <stdio.h>

using namespace std;

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

void CompileJob::__setTargetPlatform()
{
    m_target_platform = determine_platform();
}

unsigned int CompileJob::argumentFlags() const
{
    unsigned int result = Flag_None;

    for ( ArgumentsList::const_iterator it = m_flags.begin();
          it != m_flags.end(); ++it )
    {
        const string arg = it->first;
        if ( arg.at( 0 ) == '-' )
        {
	    if (arg.length() == 1)
		continue;

            if ( arg.at( 1 ) == 'g' )
            {
                if ( arg.length() > 2 && arg.at( 2 ) == '3' )
                {
                    result &= ~Flag_g;
                    result |= Flag_g3;
                }
                else
                {
                    result &= ~Flag_g3;
                    result |= Flag_g;
                }
            }
            else if ( arg.at( 1 ) == 'O' )
            {
                result &= ~( Flag_O | Flag_O2 | Flag_Ol2 );
                if ( arg.length() == 2)
			result |= Flag_O;
		else {
	            assert(arg.length() > 2);
		    if (arg.at( 2 ) == '2' )
                       result |= Flag_O2;
                    else if ( arg.at( 2 ) == '1')
                    	result |= Flag_O;
		    else if ( arg.at( 2 ) != '0')
                        result |= Flag_Ol2;
		}
            }
        }
    }
    return result;
}
