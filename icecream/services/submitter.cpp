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
submit_job (MsgChannel *c, char *filename, unsigned int fsize)
{
  GetCSMsg m1 ("gcc33", filename, fsize);
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
  unsigned int jobid = m2->job_id;
  printf ("Have to use host %s\n", m2->hostname.c_str());
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
  int remote_fd;
  struct sockaddr_in remote_addr;
  if ((remote_fd = socket (PF_INET, SOCK_STREAM, 0)) < 0)
    {
      perror ("socket()");
      return;
    }
  struct hostent *host = gethostbyname (hostname.c_str());
  if (!host)
    {
      fprintf (stderr, "Unknown host\n");
      return;
    }
  if (host->h_length != 4)
    {
      fprintf (stderr, "Invalid address length\n");
      return;
    }
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = htons (10245);
  memcpy (&remote_addr.sin_addr.s_addr, host->h_addr_list[0], host->h_length);
  if (connect (remote_fd, (struct sockaddr *) &remote_addr, sizeof (remote_addr)) < 0)
    {
      perror ("connect()");
      return;
    }
  Service *serv = new Service ((struct sockaddr*) &remote_addr, sizeof (remote_addr));
  MsgChannel *receiver_c = new MsgChannel (remote_fd, serv);
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
  delete c;
  delete serv;
}

int main (int argc, char *argv[])
{
  char *filename;
  unsigned int fsize;
  if (argc < 2)
    {
      fprintf (stderr, "submitter <filename> [<size>]\n");
      return 1;
    }
  filename = argv[1];
  fsize = 0;
  if (argc > 2)
    fsize = atoi (argv[2]);
  
  int remote_fd;
  struct sockaddr_in remote_addr;
  if ((remote_fd = socket (PF_INET, SOCK_STREAM, 0)) < 0)
    {
      perror ("socket()");
      return 1;
    }
  struct hostent *host = gethostbyname ("localhost");
  if (!host)
    {
      fprintf (stderr, "Unknown host\n");
      return 1;
    }
  if (host->h_length != 4)
    {
      fprintf (stderr, "Invalid address length\n");
      exit (1);
    }
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = htons (8765);
  memcpy (&remote_addr.sin_addr.s_addr, host->h_addr_list[0], host->h_length);
  if (connect (remote_fd, (struct sockaddr *) &remote_addr, sizeof (remote_addr)) < 0)
    {
      perror ("connect()");
      return 1;
    }
  Service *serv = new Service ((struct sockaddr*) &remote_addr, sizeof (remote_addr));
  MsgChannel *c = new MsgChannel (remote_fd, serv);
  submit_job (c, filename, fsize);
  delete c;
  delete serv;
  return 0;
}
