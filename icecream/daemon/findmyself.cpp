/*
  This file is part of the KDE libraries
  Copyright (c) 2003 Stephan Kulow <coolo@kde.org>
		2003 Michael Matz <matz@kde.org>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Library General Public
  License version 2 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Library General Public License for more details.

  You should have received a copy of the GNU Library General Public License
  along with this library; see the file COPYING.LIB.  If not, write to
  the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
  Boston, MA 02111-1307, USA.
*/

#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "findmyself.h"

using namespace std;

// TODO: use real STL :)
bool findmyself( string &arg )
{
    char path[PATH_MAX + 1];

    if (!strchr (arg.c_str(), '/'))
      {
        /* Name without any dirs.  Search $PATH.  */
	char *dirs = getenv ("PATH");
	char *d = dirs;
	path[0] = 0;
	/* So, it's slow with all the strcpy and strcats.  Who cares?  */
	while (d)
	  {
	    dirs = d;
	    d = strchr (dirs, ':');
	    if (d == dirs  /* "::" or ':' at begin --> search pwd */
	        || !*dirs) /* ':' at end --> ditto */
	      strncpy (path, arg.c_str(), PATH_MAX);
	    else
	      {
		if (d)
		  {
		    strncpy (path, dirs, d - dirs);
		    path[d - dirs] = '/';
		    path[d - dirs + 1] = 0;
		  }
		else
		  {
		    strncpy (path, dirs, PATH_MAX);
		    strncat (path, "/", PATH_MAX);
		  }
		strncat (path, arg.c_str(), PATH_MAX);
	      }
	    path[PATH_MAX] = 0;
	    if (!access (path, X_OK))
	      break;
	    if (d)
	      d++;
	  }
      }
    else
      strncpy (path, arg.c_str(), PATH_MAX);

    if (path[0] != '/')
      {
        /* Relative path, prepend pwd.  */
	if (getcwd(path, PATH_MAX))
	  {
	    strncat(path, "/", PATH_MAX);
	    strncat(path, arg.c_str(), PATH_MAX);
	  } else {
	    perror("getpwd");
	    return false;
	  }
      }

    if (!*path || access (path, X_OK))
	return false;

    arg = path;
    return true;
}
