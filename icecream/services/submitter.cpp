/*
    This file is part of Icecream.

    Copyright (c) 2004 Michael Matz <matz@suse.de>

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

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <time.h>
#include <netdb.h>
#include <string>
#include "job.h"
#include "comm.h"

using namespace std;

static void
submit_job (MsgChannel *c, char *filename)
{
  GetCSMsg m1 ("gcc33", filename, CompileJob::Lang_CXX);
  if (!c->send_msg (m1))
    return;
  Msg *_m2 = c->get_msg ();
  if (!_m2 || _m2->type != M_USE_CS)
   {
     delete _m2;
     return;
   }
  UseCSMsg *m2 = dynamic_cast<UseCSMsg *>(_m2);
  string hostname = m2->hostname;
  unsigned short port = m2->port;
  unsigned int jobid = m2->job_id;
  printf ("Have to use host %s:%d\n", m2->hostname.c_str(), port);
  printf ("Job ID: %d\n", m2->job_id);
  delete m2;
  _m2 = c->get_msg ();
  if (!_m2 || _m2->type != M_END)
    {
      delete _m2;
      return;
    }
  EndMsg em;
  if (!c->send_msg (em))
    return;
  Service *serv = new Service (hostname, port);
  MsgChannel *receiver_c = serv->channel();
  if (!receiver_c)
    return;
  CompileJob *job = new CompileJob;
  list<string> l1, l2;
  l1.push_back ("remote");
  l2.push_back ("rest");
  job->setJobID (jobid);
  job->setRemoteFlags (l1);
  job->setRestFlags (l2);
  CompileFileMsg cfm(job);
  if (!receiver_c->send_msg (cfm))
    {
      return;
    }
  delete job;
  delete serv;
}

int main (int argc, char *argv[])
{
  char *filename;
  if (argc < 2)
    {
      fprintf (stderr, "submitter <filename>\n");
      return 1;
    }
  filename = argv[1];

  MsgChannel *c = connect_scheduler ();
  if (!c)
    return 1;
  submit_job (c, filename);
  delete c;
  return 0;
}
