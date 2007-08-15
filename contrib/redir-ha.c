/* $Id$
 *
 * redir	- a utility for redirecting tcp connections
 *
 * Author:	Nigel Metheringham
 *		Nigel.Metheringham@ThePLAnet.net
 *
 * Based on, but much modified from, code originally written by
 * sammy@freenet.akron.oh.us - original header is below.
 *
 * redir is released under the GNU General Public license,
 * version 2, or at your discretion, any later version.
 *
 */

/* Redir, the code to which is below, is actually a horrible hack of my 
 * other cool network utility, daemon, which is actually a horrible hack 
 * of ora's using C sample code, 12.2.c.  But, hey, they do something.
 * (and that's the key.)
 *      -- Sammy (sammy@freenet.akron.oh.us)
 */

/* oh, incidentally, Sammy is now sammy@oh.verio.com */

/* 980601: dl9sau
 * added some nice new features:
 *
 *   --bind_addr=my.other.ip.address
 *       forces to use my.other.ip.address for the outgoing connection
 *   
 *   you can also specify, that redir listens not on all IP addresses of
 *   your system but only for the given one, i.e.:
 *      if my host has the addresses
 *        irc.thishost.my.domain  and  mail.thishost.my.domain
 *      but you want that your users do connect for the irc redir service
 *      only on irc.thishost.my.domain, then do it this way:
 *        redir irc.fu-berlin.de irc.thishost.mydomain:6667 6667
 *   my need was that:
 *        addr1.first.domain  6667 redirects to irc.first.net  port 6667
 *   and  addr2.second.domain 6667 redirects to irc.second.net port 6667
 *   while addr1 and addr2 are the same maschine and the ports can be equal.
 *
 *  enjoy it!
 *    - thomas  <thomas@x-berg.in-berlin.de>, <dl9sau@db0tud.ampr.org>
 *
 *  btw: i tried without success implementing code for the following scenario:
 *    redir --force_addr irc.fu-berlin.de 6667 6667
 *  if "--force_addr" is given and a user connects to my system, that address
 *  of my system will be used on the outgoing connection that the user
 *  connected to.
 *  i was not successful to determine, to which of my addresses the user
 *  has connected.
 */

/* Peter Skliarouk (skliaroukp@geocites.com) added possibility to
   automatically recognize failed services(hosts) and to continue maintain
   connections to "survived" services(hosts) in round-robin manner.
   If program recognized that an service failed it would try to connect
   to it every predefined number of connections
   BUGS: 
     1) Program accepts one port for all targets thus SERVICES=SERVERS
         (I just need to change the way redir takes service arguments to
          something servername:port)
     2) No inetd support
     3) No ftp support
     4) One force addr for everyone (and transparent redir).
*/

#define  VERSION "2.1.2"
#define  MAXSERVERS 10      // MAX number of servers
#define  ATTEMPTS 1000      // In case of failure attempt to reconnect every
#define  READY 0            // Service is ready
#define  EMPTYSTRING ((char*)NULL)  // empty string
#define  YES 1
#define  NO 0
#define  NOTinitialized 0
#define  FAILED -1

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <assert.h>

#ifdef USE_TCP_WRAPPERS
#include <tcpd.h>
#endif

#define debug(x)	if (dodebug) fprintf(stderr, x)
#define debug1(x,y)	if (dodebug) fprintf(stderr, x, y)
#define AND &&
#define OR ||

/* let's set up some globals... */
int dodebug = 0;
int dosyslog = 0;
unsigned char reuse_addr = 1;
unsigned char linger_opt = 0;
int isForceAddr;
char * ForceAddr;
struct sockaddr_in AddrOut;
int timeout = 0;
int ftp = 0;
int isTransProxy;
char * ident = NULL;
struct Service_ Service[MAXSERVERS];
int CurrentService;

#ifdef USE_TCP_WRAPPERS
struct request_info request;
int     allow_severity = LOG_INFO;
int     deny_severity = LOG_WARNING;
#endif /* USE_TCP_WRAPPERS */

#ifdef NEED_STRRCHR
#define strrchr rindex
#endif /* NEED_STRRCHR */

/* prototype anything needing it */
//void do_accept(int servsock, struct sockaddr_in *target);

struct Service_{
    // if it is NULL we know there is no more services in the array.
    char *sHostName;
    // later I should add PORTNUMBER - see bugs.
    int iPort;
    int ServiceTimeOut;
    struct sockaddr_in Socket;
  //    int InUse;
};

//void do_accept(int servsock, struct Service_ *Service);
void do_accept(int servsock);
int bindsock(char *addr, int port, int fail);

#ifdef NEED_STRDUP
char *
strdup(char * str)
{
    char * result;

    if (result = (char *) malloc(strlen(str) + 1))
	strcpy(result, str);

    return result;
}
#endif /* NEED_STRDUP */

void
redir_usage(char *name)
{
    fprintf(stderr,"usage:\n");
    fprintf(stderr, 
	    "\t%s --lport=<n> --cport=<n> [options]\n", 
	    name);
    fprintf(stderr, "\t%s --inetd --cport=<n>\n", name);
    fprintf(stderr, "\n\tOptions are:-\n");
    fprintf(stderr, "\t\t--lport=<n>\t\tport to listen on\n");
    fprintf(stderr, "\t\t--laddr=IP\t\taddress of interface to listen on\n");
    fprintf(stderr, "\t\t--cport=<n>\t\tport to connect to\n");
    fprintf(stderr, "\t\t--caddr=<host>\t\tremote host to connect to\n");
    fprintf(stderr, "\t\t--inetd\t\trun from inetd\n");
    fprintf(stderr, "\t\t--debug\t\toutput debugging info\n");
    fprintf(stderr, "\t\t--timeout=<n>\tset timeout to n seconds\n");
    fprintf(stderr, "\t\t--syslog\tlog messages to syslog\n");
    fprintf(stderr, "\t\t--name=<str>\ttag syslog messages with 'str'\n");
#ifdef USE_TCP_WRAPPERS
    fprintf(stderr, "\t\t            \tAlso used as service name for TCP wrappers\n");
#endif /* USE_TCP_WRAPPERS */
    fprintf(stderr, "\t\t--bind_addr=IP\tbind() outgoing IP to given addr\n");
    fprintf(stderr, "\t\t--ftp\t\tredirect passive ftp connections\n");
    fprintf(stderr, "\t\t--transproxy\trun in linux's transparent proxy mode\n");
    fprintf(stderr, "\n\tVersion %s - $Id$\n", VERSION);
    exit(2);
}

void
parse_args(int argc,
	   char * argv[],
	   struct Service_ * Services,
//	   int * target_port,
           char ** local_addr,
	   int * local_port,
	   int * timeout,
	   int * dodebug,
	   int * inetd,
	   int * dosyslog,
	   char ** bind_addr,
	   int * ftp,
	   int * transproxy)
{
     static struct option long_options[] = {
	  {"lport", required_argument, 0, 'l'},
	  {"laddr", required_argument, 0, 'a'},
	  {"cport", required_argument, 0, 'r'},
	  {"caddr", required_argument, 0, 'c'},
	  {"bind_addr", required_argument, 0, 'b'},
	  {"debug",    no_argument,       0, 'd'},
	  {"timeout",  required_argument, 0, 't'},
	  {"inetd",    no_argument,       0, 'i'},
	  {"ident",    required_argument, 0, 'n'},
	  {"name",     required_argument, 0, 'n'},
	  {"syslog",   no_argument,       0, 's'},
	  {"ftp",      no_argument,       0, 'f'},
	  {"transproxy", no_argument,     0, 'p'},
	  {0,0,0,0}		/* End marker */
     };
     
     int option_index = 0;
	 int tmp_port ;
	 int nService;
    extern int optind;
    int opt;
    struct servent *portdesc;
    char *lport = NULL;
    char *tport = NULL;
    int currentService ;
 
    *local_addr = NULL;
//    *target_addr = NULL;
//    *target_port = 0;
    *local_port = 0;

    nService =0;
    while ((opt = getopt_long(argc, argv, "disfpn:t:b:a:l:r:c:", 
			      long_options, &option_index)) != -1) {
	switch (opt) {
	case 'a':
	     *local_addr = optarg;
	     break;

	case 'l':
	     lport = optarg;
	     break;

	case 'r':
	     tport = optarg;
	     break;

	case 'c':
		{
	     Services[nService].sHostName = optarg;
	     nService++;
	     break;
		}

	case 'b':
	     *bind_addr = optarg;
	     isForceAddr= YES;
	     break;

	case 'd':
	    (*dodebug)++;
	    break;

	case 't':
	    *timeout = atol(optarg);
	    break;

	case 'i':
	    (*inetd)++;
	    break;

	case 'n':
	    /* This is the ident which is added to syslog messages */
  	    ident = optarg;
	    break;

	case 's':
	    (*dosyslog)++;
	    break;
	    
	case 'f':
	     (*ftp)++;
	     break;
	     
	case 'p':
	     (*transproxy)++;
	     break;

	default:
	    redir_usage(argv[0]);
	    exit(1);
	    break;
	}
    }

    if(tport == NULL)
    {
	 redir_usage(argv[0]);
	 exit(1);
    }

    tmp_port =  0;
    portdesc = getservbyname(tport, "tcp");
    if ( portdesc != NULL)           // succesfull lookup of service name
    {
        tmp_port = ntohs(portdesc->s_port);
    }
    else
    {
        tmp_port=atol(tport);
    }

    for(currentService = 0; currentService<nService; currentService++)
    {
        Services[currentService].iPort= tmp_port;
    }
    
    /* only check local port if not running from inetd */
    if(!(*inetd)) {
	 if(lport == NULL)
	 {
	      redir_usage(argv[0]);
	      exit(1);
	 }
	 
	 if ((portdesc = getservbyname(lport, "tcp")) != NULL) 
	      *local_port = ntohs(portdesc->s_port);
	 else
	      *local_port = atol(lport);
    } /* if *inetd */

    if (!ident) {
	 if ((ident = (char *) strrchr(argv[0], '/'))) {
	      ident++;
	 } else {
	      ident = argv[0];
	 }
    }
    
    openlog(ident, LOG_PID, LOG_DAEMON);

    return;
}

/* with the --ftp option, this one changes passive mode replies from
   the ftp server to point to a new redirector which we spawn */
void ftp_clean(int send, char *buf, unsigned long *bytes)
{

     char *port_start;
     int rporthi, lporthi;
     int lportlo, rportlo;
     int lport, rport;
     int remip[4];
     int localsock;
     int socksize = sizeof(struct sockaddr_in);
     int i;

     struct sockaddr_in newsession;
     struct sockaddr_in sockname;

     /* is this a passive mode return ? */
     if(strncmp(buf, "227", 3)) {
	  write(send, buf, (*bytes));
	  return;
     }

     /* get the outside interface so we can listen */
     if(getsockname(send, (struct sockaddr *)&sockname, &socksize) != 0) {

	  perror("getsockname");

	  if (dosyslog)
		syslog(LOG_ERR, "getsockname failed: %m");

	  exit(1);
     }

     /* parse the old address out of the buffer */
     port_start = strchr(buf, '(');

     sscanf(port_start, "(%d,%d,%d,%d,%d,%d", &remip[0], &remip[1],
	    &remip[2], &remip[3], &rporthi, &rportlo);

     /* we need to listen on a port for the incoming connection.
	we'll use this strategy.  start at 32768 for hi byte, then
	sweep using the low byte of our pid and try to bind 5 times.
	if we can't bind to any of those ports, fail */

     lporthi = 0x80;
     lportlo = getpid() & 0xf0;
     rport = (rporthi << 8) | rportlo;

     for(i = 0, localsock = -1; ((localsock == -1) && (i < 5));
	 i++, lportlo++) {

	  lport = ((lporthi << 8) | lportlo) +1; /* weird off by 1 bug */
	  localsock = bindsock(inet_ntoa(sockname.sin_addr), lport, 1);
     }

     /* check to see if we bound */
     if(localsock == -1) {
	  fprintf(stderr, "ftp: unable to bind new listening address\n");
	  exit(1);
     }

     (*bytes) = sprintf(buf, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)\n",
			sockname.sin_addr.s_addr & 0xff, 
			(sockname.sin_addr.s_addr >> 8) & 0xff, 
			(sockname.sin_addr.s_addr >> 16) & 0xff,
			sockname.sin_addr.s_addr >> 24, lporthi, lportlo);

     newsession.sin_port = htons(rport);
     newsession.sin_family = AF_INET;
     newsession.sin_addr.s_addr = remip[0] | (remip[1] << 8)
	  | (remip[2] << 16) | (remip[3] << 24);

     debug1("ftp server ip: %s\n", inet_ntoa(newsession.sin_addr));
     debug1("ftp server port: %d\n", rport);
     debug1("listening on port %d\n", lport);
     debug1("listening on addr %s\n",
	    inet_ntoa(sockname.sin_addr));


     /* now that we're bound and listening, we can safely send the new
	passive string without fear of them getting a connection
	refused. */
     write(send, buf, (*bytes));     

     /* turn off ftp checking while the data connection is active */
     ftp = 0;
//////////////////////////
/// Warning: the high-avialablility technique is not
/// applicable here, so this part of code is commented out
/// until further reviews.

//     do_accept(localsock, &newsession);
     close(localsock);
     ftp = 1;

     return;


}

void
copyloop(int insock, 
	 int outsock,
	 int timeout_secs)
{
    fd_set iofds;
    fd_set c_iofds;
    int max_fd;			/* Maximum numbered fd used */
    struct timeval timeout;
    unsigned long bytes;
    unsigned long bytes_in = 0;
    unsigned long bytes_out = 0;
    unsigned int start_time, end_time;
    char buf[4096];

    /* Record start time */
    start_time = (unsigned int) time(NULL);

    /* Set up timeout */
    timeout.tv_sec = timeout_secs;
    timeout.tv_usec = 0;

    /* file descriptor bits */
    FD_ZERO(&iofds);
    FD_SET(insock, &iofds);
    FD_SET(outsock, &iofds);

    
    if (insock > outsock) {
	max_fd = insock;
    } else {
	max_fd = outsock;
    }

    debug1("Entering copyloop() - timeout is %d\n", timeout_secs);
    while(1) {
	(void) memcpy(&c_iofds, &iofds, sizeof(iofds));


	if (select(max_fd + 1,
		  &c_iofds,
		  (fd_set *)0,
		  (fd_set *)0,
		  (timeout_secs ? &timeout : NULL)) <= 0) {
	    /*	    syslog(LLEV,"connection timeout: %d sec",timeout.tv_sec);*/
	    break;
	}

	if(FD_ISSET(insock, &c_iofds)) {
	    if((bytes = read(insock, buf, sizeof(buf))) <= 0)
		break;
	    if(write(outsock, buf, bytes) != bytes)
		break;
	    bytes_out += bytes;
	}
	if(FD_ISSET(outsock, &c_iofds)) {
	    if((bytes = read(outsock, buf, sizeof(buf))) <= 0)
		break;
	    /* if we're correcting for PASV on ftp redirections, then
	       fix buf and bytes to have the new address, among other
	       things */
	    if(ftp)
		 ftp_clean(insock, buf, &bytes);
	    else 
		 if(write(insock, buf, bytes) != bytes)
		      break;
	    bytes_in += bytes;
	}
    }
    debug("Leaving main copyloop\n");

/*
    setsockopt(insock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));
    setsockopt(insock, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(SO_LINGER)); 
    setsockopt(outsock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));
    setsockopt(outsock, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(SO_LINGER)); 
*/

    shutdown(insock,0);
    shutdown(outsock,0);
    close(insock);
    close(outsock);
    debug("copyloop - sockets shutdown and closed\n");
    end_time = (unsigned int) time(NULL);
    debug1("copyloop - connect time: %8d seconds\n", end_time - start_time);
    debug1("copyloop - transfer in:  %8ld bytes\n", bytes_in);
    debug1("copyloop - transfer out: %8ld bytes\n", bytes_out);
    if (dosyslog) {
	syslog(LOG_NOTICE, "disconnect %d secs, %ld in %ld out",
	       (end_time - start_time), bytes_in, bytes_out);
    }
    return;
}

// Socket initialization for connection to real server.
int InitializeSocket
(
 int isTransProxy,
 int isForceAddr,
 struct sockaddr_in *AddrOut,
 struct sockaddr_in *AddrClient
)
{
    int targetsock;
    //  int val;

    targetsock = socket(AF_INET, SOCK_STREAM, 0);
    // set non-block mode
    //  val = 0;
    //  ioctl(targetsock,BLOCKING,&val);
    if (targetsock==FAILED)
    {
        if (dodebug)
	{
	    perror("target: socket");
	}
	if (dosyslog)
	{
	    syslog(LOG_ERR, "socket failed: %m");
	}
	exit(1);
    }
    if(isTransProxy==YES) {
        memcpy(AddrOut, AddrClient, sizeof(struct sockaddr_in));
	AddrOut->sin_port = 0;   // To choose port yourself.
    }
          
    if ((isForceAddr==YES) OR (isTransProxy==YES))
    {
      
        /*
	  this only makes sense if an outgoing IP addr has been forced;
	  at this point, we have a valid targetsock to bind() to.. 
	  also, if we're in transparent proxy mode, this option
	  never makes sense 
	*/

        if (bind(targetsock, (struct  sockaddr  *) AddrOut,
		 sizeof(struct sockaddr_in)) < 0)
	{
	    perror("bind_addr: cannot bind to forced outgoing addr");

	    if (dosyslog)
	      syslog(LOG_ERR, "bind failed: %m");

	    _exit(1);
	}
	debug1("outgoing IP is %s\n", inet_ntoa(AddrOut->sin_addr));
    }

    return(targetsock);
}

/* lwait for a connection and move into copyloop...  again,
   passive ftp's will call this, so we don't dupilcate it. */

//do_accept(int servsock, struct sockaddr_in *target)
void
do_accept(int servsock)
{
    int clisock;
    int targetsock;
    struct sockaddr_in client;
    int clientlen = sizeof(client);
    struct sockaddr_in * target;
    int isConnect;
    int ServiceToTry;
    int isConnected;
    int isMoreServices;
    // struct Service_ *Service           // global structure
    int SonCurrentService;
    int isVacantSocket;
    
    debug("top of accept loop\n");
    if ((clisock = accept(servsock, (struct  sockaddr  *) &client, 
			  &clientlen)) < 0) 
    {
        perror("server: accept");
	if (dosyslog)
	{
	    syslog(LOG_ERR, "accept failed: %m");
	}
	exit(1);
    }
     //////////
     /////////    setsockopt(targetsock,SOL_SOCKET,SOCKET_KEEPALIVE,&val)    
    debug1("peer IP is %s\n", inet_ntoa(client.sin_addr));
    debug1("peer socket is %d\n", client.sin_port);

    /* CurrentService is global variable which shows
       number of service to which last connection attempt was made */

    /* The algorithm is:
       Get request
       Find first service not "in use" (nobody tries to connect to it)
       Mark the service "in use"
       Fork to serve the request (telling to grandchild to connect to that service)
       Grandchild: if connection failed - mark the service as "failed", try next
         service not "in use" 
    */


    // Looking for service not "in use"
    ServiceToTry=CurrentService;
    isMoreServices=YES;         // to avoid deadlock
    isVacantSocket=NO;

    while((isMoreServices==YES) AND (isVacantSocket==NO))
    {
	if (Service[ServiceToTry].sHostName==EMPTYSTRING)
	{
	    debug("Reached end of service's list\n");
	    ServiceToTry=0;
	    continue;
	}
	if (Service[ServiceToTry].ServiceTimeOut>0)
	{
	    Service[ServiceToTry].ServiceTimeOut--;
/*
	    if (Service[ServiceToTry].ServiceTimeOut==0)
	    {
#pragma _CRI guard
	        Service[ServiceToTry].InUse=NO;
#pragma _CRI endguard
	    }
*/
	    ServiceToTry++;
	    if (ServiceToTry==CurrentService)
	      isMoreServices=NO;
	    continue;
	}
/*
	if (Service[ServiceToTry].InUse==YES)
	{
	    ServiceToTry++;
	    // we should not set isMoreServices to NO if it failed -
	    // could be this is the only Service left and it has huge queue
	    // of connections to serve...
	    if (ServiceToTry==CurrentService)
	    {
	      // let's sleep for a second...
	      sleep(1);
	    }
	    continue;
	}
*/
	// Found vacant socket :-)
	CurrentService++;  // next time try to use another service
	if (Service[CurrentService].sHostName==EMPTYSTRING)
	{
	    debug("Reached end of service's list\n");
	    CurrentService=0;
	}	
	debug1("Next service will be: %d\n",CurrentService);

	isVacantSocket=YES;
    }
    if (isMoreServices==NO)
    {
        if (dosyslog)
	  syslog(LOG_ERR, "No available services!");
	debug("No avialable services!\n");
	return;
    }

    assert(isVacantSocket==YES);

     /*
      * Double fork here so we don't have to wait later
      * This detaches us from our parent so that the parent
      * does not need to pick up dead kids later.
      *
      * This needs to be done before the hosts_access stuff, because
      * extended hosts_access options expect to be run from a child.
      */
     switch(fork())
     {
     	case -1: /* Error */
     		perror("(server) fork");

     		if (dosyslog)
     			syslog(LOG_ERR, "(server) fork failed: %m");

     		_exit(1);
     	case 0:  /* Child */
     		break;
     	default: /* Parent */
     	{
     		int status;
	  
     		/* Wait for child (who has forked off grandchild) */
     		(void) wait(&status);

     		/* Close sockets to prevent confusion */
     		close(clisock);
     		return;
     	}
     }

     /* We are now the first child. Fork again and exit */
	  
     switch(fork())
     {
     	case -1: /* Error */
     		perror("(child) fork");

     		if (dosyslog)
     			syslog(LOG_ERR, "(child) fork failed: %m");

     		_exit(1);
     	case 0:  /* Child */
     		break;
     	default: /* Parent */
     		_exit(0);
     }
     
     /* We are now the grandchild */

     /* Grandchild: if connection to ServiceToTry
	failed - mark the service as "failed", try next
	service not "in use" */

#ifdef USE_TCP_WRAPPERS
     request_init(&request, RQ_DAEMON, ident, RQ_FILE, clisock, 0);
     sock_host(&request);
     sock_hostname(&request);
     sock_hostaddr(&request);

     if (!hosts_access(&request)) {
     	refuse(&request);
		_exit(0);
     }

    if (dosyslog){
        syslog(LOG_INFO, "accepted connect from %s", eval_client(&request));
    }
#endif /* USE_TCP_WRAPPERS */

    targetsock=NOTinitialized;
    target=NULL;

    SonCurrentService=ServiceToTry;
    isMoreServices=YES;         // to avoid deadlock in trying the services
    isConnected=NO;

    while((isMoreServices==YES) AND (isConnected==NO))
    {
	if (Service[ServiceToTry].sHostName==EMPTYSTRING)
	{
	    debug("Son: Reached end of service's list\n");
	    ServiceToTry=0;
	    continue;
	}
        if (targetsock==NOTinitialized)
	{
	    targetsock=InitializeSocket(isTransProxy,isForceAddr,&AddrOut,&client);
	}
	if (Service[ServiceToTry].ServiceTimeOut>0)
	{
	    ServiceToTry++;
	    if (ServiceToTry==SonCurrentService)
	      sleep(1);
	    continue;
	}
	// Trying to connect...
	target=&(Service[ServiceToTry].Socket);
// let's mark the socket "in use"
	
	isConnect=connect(targetsock, (struct  sockaddr  *) target,
			  sizeof(struct sockaddr_in));
	//////////////////////////////////////
	// Keep Alive - setsockopt(targetsock,SOL_SOCKET,SOCKET_KEEPALIVE,&val)
	if (isConnect==FAILED)
	{
	    // we need to rebuild socket
	    close(targetsock);
	    targetsock=NOTinitialized;
	    //	    perror("target: connect");
	    if (dosyslog)
	      syslog(LOG_ERR, "target: connect failed: %m");
	    debug1("Service %d failed\n",ServiceToTry);
	    Service[ServiceToTry].ServiceTimeOut=ATTEMPTS;
	    ServiceToTry++;
	    if (ServiceToTry==SonCurrentService)
	    { 
	        // We couldn't just terminate the thread - we have
	        // client on hold... :-(
	        // Let's sleep and after that will see what we could do.
	        sleep(1);
	    }
	    continue;
	}
	// Connection established.
	isConnected=YES;
    }
     
    debug1("connected to %s\n", inet_ntoa(target->sin_addr));

    /* thanks to Anders Vannman for the fix to make proper syslogging
       happen here...  */

    if (dosyslog) {
        char tmp1[20], tmp2[20];
	strcpy(tmp1, inet_ntoa(client.sin_addr));
	strcpy(tmp2, inet_ntoa(target->sin_addr));
	
	syslog(LOG_NOTICE, "connecting %s/%d to %s/%d",
	       tmp1, client.sin_port,
	       tmp2, target->sin_port);
    }
    copyloop(clisock, targetsock, timeout);
    exit(0);	/* Exit after copy */
}

/* bind to a new socket, we do this out here because passive-fixups
   are going to call it too, and there's no sense dupliciting the
   code. */
/* fail is true if we should just return a -1 on error, false if we
   should bail. */

int bindsock(char *addr, int port, int fail) 
{

     int servsock;
     struct sockaddr_in server;
     
     /*
      * Get a socket to work with.  This socket will
      * be in the Internet domain, and will be a
      * stream socket.
      */
     
     if ((servsock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	  if(fail) {
	       return -1;
	  }
	  else {
	       perror("server: socket");

	       if (dosyslog)
				syslog(LOG_ERR, "socket failed: %m");

	       exit(1);
		}
     }
     
     server.sin_family = AF_INET;
     server.sin_port = htons(port);
     if (addr != NULL) {
	  struct hostent *hp;
	  
	  debug1("listening on %s\n", addr);
	  if ((hp = gethostbyname(addr)) == NULL) {
	       fprintf(stderr, "%s: cannot resolve hostname.\n", addr);
	       exit(1);
	  }
	  memcpy(&server.sin_addr, hp->h_addr, hp->h_length);
     } else {
	  debug("local IP is default\n");
	  server.sin_addr.s_addr = htonl(inet_addr("0.0.0.0"));
     }
     
     setsockopt(servsock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));
     setsockopt(servsock, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(SO_LINGER)); 
     
     /*
      * Try to bind the address to the socket.
      */
     
     if (bind(servsock, (struct  sockaddr  *) &server, 
	      sizeof(server)) < 0) {
	  if(fail) {
	       close(servsock);
	       return -1;
	  } else {
	       perror("server: bind");

	       if (dosyslog)
				syslog(LOG_ERR, "bind failed: %m");

	       exit(1);
		}
     }
     
     /*
      * Listen on the socket.
      */
     
     if (listen(servsock, 10000) < 0) {
	  if(fail) {
	       close(servsock);
	       return -1;
	  } else {
	       perror("server: listen");

	       if (dosyslog)
				syslog(LOG_ERR, "listen failed: %m");

	       exit(1);
		}
     }
     
     return servsock;
}

void BuildSocket(struct sockaddr_in *Socket,char *sHostName ,int iPort )
{
    char* target_ip ;

    Socket->sin_family = AF_INET;
    Socket->sin_port = htons(iPort);
    if (sHostName != NULL) {
	struct hostent *hp;

	debug1("target is %s\n", sHostName);
	if ((hp = gethostbyname(sHostName)) == NULL) {
	    fprintf(stderr, "%s: host unknown.\n", sHostName);
	    exit(1);
	}
	memcpy(&Socket->sin_addr, hp->h_addr, hp->h_length);
    } else {
	debug("target is localhost\n");
	Socket->sin_addr.s_addr = htonl(inet_addr("0.0.0.0"));
    }

    if (dodebug)
    {
        target_ip = NULL;
	target_ip = strdup(inet_ntoa(Socket->sin_addr));

	if(target_ip)
	{
	    debug1("target IP address is %s\n", target_ip);
	    debug1("target port is %d\n", iPort);
	}
    }
}

void BuildServersSockets(struct Service_* Service){
    // Seting up Service's sockets
    int nService=0;
    while(Service[nService].sHostName != EMPTYSTRING){
 	BuildSocket(&(Service[nService].Socket),Service[nService].sHostName,Service[nService].iPort);
	nService++;
    }
}

void InitServices(struct Service_* Service)
{
	int nService;

	if(Service)
	{
	   for(nService=0; nService<MAXSERVERS; nService++)
	   {
		 Service[nService].sHostName=EMPTYSTRING;
		 Service[nService].ServiceTimeOut=READY;
	   }
	}
}

int
main(int argc, char *argv[])
{
//    struct sockaddr_in target;
//    int target_port;
    char *local_addr;
    int local_port;
    int inetd = 0;
//    char * target_ip;
    char * ip_to_target;

    debug("parse args\n");

    InitServices(Service);
    isTransProxy=NO;
    isForceAddr=NO;
    ForceAddr=NULL;
//    parse_args(argc, argv, Service, &target_port, &local_addr, 
    parse_args(argc, argv, Service, &local_addr, 
	       &local_port, &timeout, &dodebug, &inetd, &dosyslog, &ForceAddr,
	       &ftp, &isTransProxy);

    BuildServersSockets(Service);
    /* Set up outgoing IP addr (optional);
     * we have to wait for bind until targetsock = socket() is done
     */
    if ((isForceAddr==YES) AND (isTransProxy==NO)) {
	struct hostent *hp;

	fprintf(stderr, "bind_addr is %s\n", ForceAddr);
    	AddrOut.sin_family = AF_INET;
    	AddrOut.sin_port = 0;
	if ((hp = gethostbyname(ForceAddr)) == NULL) {
	    fprintf(stderr, "%s: cannot resolve forced outgoing IP address.\n", ForceAddr);
	    exit(1);
        }
	memcpy(&AddrOut.sin_addr, hp->h_addr, hp->h_length);

        ip_to_target = strdup(inet_ntoa(AddrOut.sin_addr));
        debug1("IP address for target is %s\n", ip_to_target);
    }
           
    if (inetd) {
//////////////////////
///   Warning!!!
///   INETD part not affected by the patch.
///   It probably doesn't work at all!!!!
//////////////////////
	int targetsock;
	struct sockaddr_in client;
	int client_size = sizeof(client);

       exit(1);   // to prevent catastrophes
#ifdef USE_TCP_WRAPPERS
	request_init(&request, RQ_DAEMON, ident, RQ_FILE, 0, 0);
	sock_host(&request);
	sock_hostname(&request);
	sock_hostaddr(&request);
	
	if (!hosts_access(&request))
		refuse(&request);
#endif /* USE_TCP_WRAPPERS */

	if (!getpeername(0, (struct sockaddr *) &client, &client_size)) {
	  debug1("peer IP is %s\n", inet_ntoa(client.sin_addr));
	  debug1("peer socket is %d\n", client.sin_port);
	}
	if ((targetsock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	    perror("target: socket");

	    if (dosyslog)
			syslog(LOG_ERR, "targetsock failed: %m");

	    exit(1);
	}

	if(isTransProxy) {
	     memcpy(&AddrOut, &client, sizeof(struct sockaddr_in));
	     AddrOut.sin_port = 0;
	}

	if ((isForceAddr==YES) OR (isTransProxy==YES)) {
	    /* this only makes sense if an outgoing IP addr has been forced;
	     * at this point, we have a valid targetsock to bind() to.. */
	     if (bind(targetsock, (struct  sockaddr  *) &AddrOut, 
		     sizeof(AddrOut)) < 0) {
	          perror("bind_addr: cannot bind to forcerd outgoing addr");
				 
	          if (dosyslog)
					syslog(LOG_ERR, "bind failed: %m");
				 
	          exit(1);
	     }
	     debug1("outgoing IP is %s\n", inet_ntoa(AddrOut.sin_addr));
        }

///////////////////
/// Warning!!!
/// The following code is completely commented out
/// because the whole INETD part was not patched.
 
/*	if (connect(targetsock, (struct sockaddr *) &target, 
		    sizeof(target)) < 0) {
 	    perror("target: connect");

	    if (dosyslog)
	 		syslog(LOG_ERR, "connect failed: %m");

	    exit(1);
	}

	if (dosyslog) {
	    syslog(LOG_NOTICE, "connecting %s/%d to %s/%d",
		   inet_ntoa(client.sin_addr), client.sin_port,
		   target_ip, target.sin_port);
	}
*/

	/* Just start copying - one side of the loop is stdin - 0 */
	copyloop(0, targetsock, timeout);
        /* End of inetd invocation */
    } else {
        /* if it is standalone daemon - continue here */
        /* Let's listen for incoming connections */
	int servsock;
	
	if(local_addr)
	     servsock = bindsock(local_addr, local_port, 0);
	else
	     servsock = bindsock(NULL, local_port, 0);

	/*
	 * Accept connections.  When we accept one, ns
	 * will be connected to the client.  client will
	 * contain the address of the client.
	 */

	CurrentService=0;	// global variable
	while (1)
	{
//	     do_accept(servsock, &target);
	    do_accept(servsock);   // Service is global structure
	}
    }

    /* this should really never be reached */

    exit(0);

}


