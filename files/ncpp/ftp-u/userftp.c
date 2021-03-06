#/*
Module Name:
	userftp.c

Installation:
	cc -O -f -s userftp.c -lj;:	compile the standard version
	su cp a.out /usr/bin/ftp;:	move it into the system library
	ed - userftp.c;:		make a version with a big buffer
/#define.*termsize/s,termsize.*,termsize 8192,
w bigftp.c
q
	cc -O -f -s bigftp.c -lj;:	compile the big version
	su cp a.out /usr/bin/bigftp;:	move it into the system library
	rm a.out bigftp.c;:		remove evidence

Function:

Globals contained:

Routines contained:

Modules referenced:

Modules referencing:

Compile time parameters and effects:

Module History:
*/
#
/* defines */

#define	termsize	1024
#define	CR		015
#define LF		012
#define ECHO		010

/* defines for the type field of parameterized open to network */

#define	o_direct	01	/* icp |~  direct */
#define o_server	02	/* user | server */
#define o_init		04	/* listen | init */
#define o_specific	010	/* general  | specific */
#define o_duplex	020	/* simplex | duplex */
#define o_relative	040	/* absolute | relative */

/* arrays and structs */

int  tty[3];			/* holds tty state bits */
char termbuf[ termsize ];	/* data buffer into which most data goes */
char inputbuffer[512];		/* main buffer for primary input */
char hostname[80];		/* permanently holds full host name for data connections */
char *comarr[]
{
	"get",
	"put",
	"abort",
	"ascii",
	"bin",
	"tenex",
	"bye",
	"end",
	"log",
	"!",
	   0
};

/* To pacify V7 C compiler, sigh */
extern net_write(),get(),put(),abort(),asciimode(),binmode(),tenexmode(),
	done(),log(),unixcommand();

int (*funcarr[])()
{
	&net_write,
	&get,
	&put,
	&abort,
	&asciimode,
	&binmode,
	&tenexmode,
	&done,
	&done,
	&log,
	&unixcommand
};

struct openparams		/* struct for parameterized network opens */
{

	char o_op;		/* opcode for kernel & daemon - unused here */
	char o_type;		/* type for connection see defines below */
	int o_id;		/* id of file for kernel & daemon - unused here */
	int o_lskt;		/* local socket number either abs or rel */
	int o_fskt[2];		/* foreign skt either abs or rel */
	char o_frnhost;		/* foreign host number */
	char o_bsize;		/* bytesize for the connection */
	int o_nomall;		/* nominal allocation for the connection */
	int o_timeo;		/* number of secs before time out */
	int  o_relid;		/* fid of file to base a data connection on */

} openparams;

/* integer decs */

int termbytes;			/* number of bytes currently in termbuf */
int local_data_fid;		/* file descriptor of local file assoc w/ data conn */
int for_data_fid;		/* file descriptor of data connection */
int netfid;			/* file descriptor for telnet command connection */
int childpid;			/* process id of child data connection process */
int ascii	1;			/* flag =1 says crlf->lf or lf->crlf; 0 no map */
char *mapspace;			/* pointer to extra space for lf->crlf */
int weshould	1;		/* says when to stop */
int wantlf	0;		/* used by crlf_to_lf */
long bytestransferred;	/* for calculating statisics */
long starttime;		/* time at which an xfer started */
int gottime;
long stoptime;		/* time at which an xfer completed */

char *inputptr;			/* primary input - current pointer */
int inputcount;			/* primary input - count remaining */

extern int errno;		/* extern for perror */
/*name:
	user ftp 

function:
	to transfer data files to/from the local host to a foreign server

algorithm:
	open the telnet command connection to the host specified
	( if the hostname is not specified as a param, get it )
	forever, 
		read any awaiting data from terminal

		if any data was gotten see if it is a 
		known command( comarr ) if so do what 
		is needed, otherwise tack on a crlf and
		send it down the telnet command connection.

		see if there is any data to be read from the net
		if so read it.

		if any data was read in from the net, write it
		on the controling terminal making crlf into lf

parameters:
	host name specified on the command line

returns:
	when done or error

globals:
	termbuf=
	termbytes=
	argc
	argv=
	weshould

calls:
	getnullstr
	connftpserver
	term_read
	decodeanycommands
	net_read
	term_write

called by:
	user typing 'ftp <hostname>' on command line

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

main( argc,argv )
char *argv[];
{

	/* if no hostname given ask for it */
	if( argc == 1 )
	{
		printf("Host: ");
		getnullstr(0);	/* get hostname null terminated */
		argv[1] = termbuf;	/* make pointer to hostname */
	}

	printf("Attempting Connection to %s\n",argv[1]);
	/* try and open the connection to the ftp server */
	connftpserver( argv[1] );
	printf("Connection Open\n");

	/* if we get here, the connection opened ok */
	if( fork() == 0 )
	{	/* this guy reads from net and writes to terminal */
		/* I don't want to die just because the user trys to
		   abort an inferior shell */
		signal(2,1);

		while( 1 )
		{
			/* get data from net */
			net_read();
			/* stick out on term */
			term_write();
		}
	}
	else
		/* this fella reads from term and writes to net */
		while( weshould )
		{
			/* get data from term */
			term_read( 0 );
			/* look for commands and pass data to net */
			decodeanycommands();
		}
}

/*name:
	connftpserver

function:
	make a connection to socket 3 of host specifed in hname

algorithm:
	build '/dev/net/<hostname>' in global hostname 
	setup for standard telnet connection to socket 3
	open the connection
		if error say so and exit


parameters:
	null terminated string containing the hostname to be
	connected to.

returns:
	'/dev/net/<hostname>' in hostname for use in opening data
	connections.

	netfid has the file descriptor of the opened connection

globals:
	hostname=
	netfid=
	openparams.o_fskt[1]=

calls:
	strmove
	open (sys)
	printf (sys)

called by:
	main

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

connftpserver( hname )
char *hname;
{

	/* build host file name */
	strmove( hname, strmove( "/dev/net/",hostname ) );

	/* say connection is to socket 3 */
	openparams.o_fskt[1] = 3;

	/* make a connection to socket 3 */
	netfid = open( hostname,&openparams );
	if( netfid < 0 )
	{
		/* no soap */
		printf("Host is unavailable\n");
		exit();			/* exit program */
	}
}

/*name:
	term_read

function:
	Get a "\n" delimited line from the primary input source,
	put it in the buffer termbuf, and set the global variable
	termbytes to the character count in that line.

algorithm:
	Do reads into a main buffer, then take them one at a time,
	and transfer them to termbuf, doing new reads when necessary.
	The reason that the reads are not done into termbuf is that
	if the primary input is not a tty, a read may yield multiple
	lines, but termbuf should only get loaded with one line at
	a time.

parameters:
	starting index into termbuf to put data 

returns:
	bytes read in termbuf and number read in termbytes

globals:
	termbuf=
	termbytes=
	inputcount	Number of chars remaining in main buffer
	inputbuffer	The main buffer itself
	inputptr	Pointer to the next character in main buffer

calls:
	read (sys)

called by:
	main
	getnullstr

history:
	initial coding 6/9/75 by S. F. Holmgren
	modified for non-tty input 10/16/75 by M. Kampe
	modified to take index 9/27/76 by S. F. Holmgren

*/

term_read( indx )
{
	register int incnt;
	register char *outptr;
	register char *inptr;

	incnt = inputcount;
	inptr = inputptr;

	for(outptr = termbuf+indx; outptr < &termbuf[termsize-1]; )
	{	if (incnt <= 0)
		{	if ((incnt = read(0,inputbuffer,512)) <= 0)
			{	weshould = 0;
				break;
			}
			inptr = inputbuffer;
		}

		incnt--;
		if ((*outptr++ = *inptr++) == '\n') break;
	}

	inputcount = incnt;
	inputptr = inptr;
	termbytes = outptr - termbuf;
	return;
}

/*name:
	decodeanycommands

function:
	if there was data from terminal, see if it is a known
	command and if so call the correct procedure( funcarr )

algorithm:
	see function

parameters:
	termbytes (global)

returns:
	nothing

globals:
	termbytes
	funcarr

calls:
	net_write (thru funcarr)
	get	   (thru funcarr)
	put	   (thru funcarr)
	asciimode	(thru funcarr)
	binmode		(thru funcarr)
	tenexmode	(thru funcarr)
	whichcomm

called by:
	main

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

decodeanycommands()
{

		/* call either data write or a command procedure */
		(*funcarr[ whichcomm() ]) ();
}

/*name:
	whichcomm

function:
	scan thru comarr and try and find a match between
	user input and any of the commands
	if one is found, return its relative index + 1

algorithm:
	while there are entries in comarr
		does this one compare to user input
			if so return relative entry

	been all thru and not found anything so send off

parameters:
	termbuf ( global )

returns:
	relative index into comarr in which string match was found
	otherwise zero

globals:
	comarr
	termbuf

calls:
	compare

called by:
	decodeanycommands

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

whichcomm()
{

	register char **comp;
	register char **startp;

	comp = startp = &comarr;

	/* while there are strings in comarr */
	while( *comp )
		if( compare( *comp++,termbuf ) )	/* did we find one? */
			return( comp - startp );	/* return its index */
	return( 0 );					/* no takers */
}

/*name:
	net_write

function:
	tack a crlf on the end of the string
	send the data to the command telnet connection
	if there is an error then exit

algorithm:
	see function

parameters:
	termbuf (global)
	termbytes (global)

returns:
	if it does the write went ok otherwise the prog exits

globals:
	termbytes
	termbuf=

calls:
	write (sys)
	printf (sys)

called by:
	decodanycommands

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

net_write()
{

	/* expects a null terminated string to be in termbuf */
	termbuf[ termbytes-1 ] = CR;	/* overwrite last char with cr */
	termbuf[ termbytes ] = LF;	/* add a line feed */

	if( write( netfid,termbuf,++termbytes ) < 0 )
	{
		printf("Host has closed the Connection\n");
		weshould = 0;
	}
}

/*name:
	get

function:
	open a data connection to the foreign host
	then spawn a process that reads from the net
	data connection and writes to a local file

algorithm:
	get local file name
	try and create the file
		if cant create say so and return
	get foreign file name
	formulate the retr command and send to network
	then open the data connection
	fork
		child
		while read from net 
			do any crlf to lf translation needed
			write to local file

parameters:
	local file name -- gotten from user
	foreign path name -- gotten from user

returns:
	nothing

globals:
	termbuf=
	termbytes=
	local_data_fid=
	for_data_fid=
	childpid=
	openparams.type=
	openparams.relid=
	openparmas.lskt=
	openparams.fskt[1]=
	starttime and stoptime	for statistics time stamps

calls:
	strmove
	net_write
	printf (sys)
	getnullstr
	creat (sys)
	write (sys)
	open (sys)
	close (sys)
	fork (sys)
	time (sys)
	crlf_to_lf

called by:
	decodeanycommands

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

get()
{

	/* ask for local file name */
	printf(" Local File Name: ");
	getnullstr(0);			/* get file name null termed */

	/* try and open the file first, may be a special file */
	local_data_fid = open( termbuf,1 );
	/* if open failed and if create fails */
	if( local_data_fid < 0 && (local_data_fid = creat( termbuf,0666)) < 0 )
	{
		/* cant create file */
		printf(" Cant create %s\n",termbuf);
		return;
	}

	/* get foreign path name */
	printf(" Foreign File Name: ");

	/* move in first part of retr command */
	strmove("retr ",termbuf);
	getnullstr(5);			/* get file name null termed */
	net_write();			/* send data to network */

	/* set  up connection on default u+4 and s+5 */
	openparams.o_lskt = 4;		/* relative 4 off of local base */
	openparams.o_fskt[1] = 0;	/* any connection to 4 off local base */
	openparams.o_relid = netfid;	/* saw which local base to use */
	openparams.o_type = (o_relative|o_direct);
	openparams.o_nomall = 2048;	/* up allocation so stuff moves faster */

	/* fork and let child do the data handling */
	if( (childpid = fork()) == 0 )
	{
		bytestransferred = 0;
		setsignals();		/* so user knows when error occurs */
		/* try and open the data connection */
		if ( (for_data_fid = open( hostname,&openparams )) < 0 )
		{
			/* no soap */
			printf(" Cant Open Data Connection\n");
			return;
		}
		gottime = 0;
		sleep (2); /* give messages time to clear */
		while( (termbytes = read( for_data_fid,termbuf,termsize )) > 0 )
		{
			if (!gottime) {
				gottime++;
				time (&starttime);
				printf ("First byte of data received\n");
			}
			/* if i should map crlf to lf do it */
			if( ascii )
				crlf_to_lf(local_data_fid);
			else	write( local_data_fid,termbuf,termbytes );
			/* update number of bytes received */
			bytestransferred =+ termbytes;

		}
		time(&stoptime);		/* and get a closing timestamp */
		statistics();		/* say a little something for the folks */
		exit();			/* make child go away */
	}
	close( local_data_fid );
}

/*name:
	put

function:
	get data from local filing system and send to data connection
	based on telnet connection already open.

algorithm:
	get local pathname
	try and open file
		if cant say so and return
	get foreign pathname
	formulate and send 'stor' command
	open data connection
	spawn child which will read from local file
	and write to foreign file until eof

parameters:
	local file name -- gotten from user 
	foreign file name -- gotten from user
	telnet command connection already open

returns:
	nothing

globals:
	termbuf=
	termbytes=
	openparams.type=
	openparams.relid=
	openparams.lskt=
	openparams.frnskt[1]=
	starttime and stoptime	for a statistics time stamp

calls:
	strmove
	net_write
	getnullstr
	open (sys)
	printf (sys)
	read (sys)
	write (sys)
	exit (sys)
	close (sys)
	fork (sys)
	time (sys)

called by:
	decodeanycommands

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

put()
{

	printf(" Local File Name: ");
	getnullstr(0);			/* get null termed fname */
	if( (local_data_fid = open( termbuf,0 )) < 0 )
	{						/* cant open local file */

		printf("Cant open %s\n",termbuf);
		return;
	}

	/* get foreign pathname */
	printf(" Foreign File Name: ");
	strmove( "stor ",termbuf );	/* move in first part of comm */
	getnullstr(5);			/* get null termed fname */
	net_write();			/* send it off to net conn */

	/* formulate the parameters for the data connection */
	openparams.o_type = (o_relative|o_direct);	/* set type */
	openparams.o_relid = netfid;			/* me the already open */
	openparams.o_lskt = 5;				/* u+5 from already open */
	openparams.o_fskt[1] = 0;			/* any conn 4 off local sock */

	/* start up a process to read from local and send to net */
	if( (childpid = fork()) == 0 )
	{
		bytestransferred = 0;	/* init number of bytes send */
		setsignals();		/* so user knows if error occured */
		/* try and open data connection */
		if( (for_data_fid = open( hostname,&openparams )) < 0 )
		{
			/* no soap */
			printf(" Cant Open Data Connection\n");
			return;
		}
		time(&starttime);	/* get a starting time stamp */
		gottime = 0;
		while( (termbytes=read(local_data_fid,termbuf,termsize)) > 0 )
		{
			gottime++;
			if ( ascii )		/* should i map */
				lf_to_crlf();	/* then do the map */
			else
				if( write( for_data_fid,termbuf,termbytes ) <= 0  )
				{
					printf(" Host Closed Data Connection\n");
					printf(" Aborting Transfer\n");
					break;
				}
			bytestransferred =+ termbytes;
		}
		time(&stoptime);		/* and get an ending time stamp */
		statistics();		/* tell the folks at home what you did */
		exit();					/* kill this process */
	}
	close( local_data_fid );
}

/*name:
	lf_to_crlf

function:
	to translate Line Feeds in termbuf into CRLF combinations

algorithm:
	copy the contents of termbuf into space twice as large
	everytime an lf is fund make it into a crlf

parameters:
	termbbuf
	termbytes

returns:
	nothing

globals:
	termbytes
	termbuf

calls:
	nothing

called by:
	put

history:
	initial coding 6/9/75 by S. F. Holmgren
*/

lf_to_crlf()
{

	register char *inp;
	register char *outp;
	register count;

	if( mapspace == 0 )
		mapspace = alloc ( termsize * 2 );

	inp = termbuf;
	outp = mapspace;
	count = termbytes;
	count++;

	while( --count )
	{
		if( *inp == LF )
		{	*outp++ = CR;
			termbytes++;
		}
		*outp++ = *inp++;
	}

	write( for_data_fid,mapspace,termbytes );
}
/*name:
	crlf_to_lf

function:
	to take the chars in termbuf, and translate any crlf character
	combinations into lf.

algorithm:
	thru the number of bytes in termbuf
		if there is a CR
			if there is an LF
				then copy the LF over the CR
				say there is one less byte
				in both termbytes and byte count

parameters:
	termbuf=
	termbytes=

returns:
	termbuf translated
	termbytes with the correct number of bytes

globals:
	termbuf=
	termbytes=

calls:
	nothing

called by:
	get
	term_write

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

crlf_to_lf(outfile)
{

	register char *inp;
	register char *outp;
	register count;

	inp = outp = termbuf;
	count = termbytes;
	count++;		/* so can do auto dec in while */

	if( wantlf && *inp != LF )
		write( outfile,"\r",1 );
	wantlf = 0;
	/* thru the number of bytes */
	while( --count )
	{
		if( (*outp = *inp++) == CR )
		{
			/* is it crlf sequence */
			if( count-1 > 0 )	/* can we get another char */
				if( *inp == LF )	/* yes then wipe CF */
				{
					*outp = *inp++;
					count--;
					termbytes--;
				}
				else
					wantlf = 0;
			else
			{
				wantlf++;		/* say we need line feed */
				termbytes--;			/* say one less to write */
			}
		}
		outp++;					/* get next char */
	}
	write( outfile,termbuf,termbytes );
}

/*name:
	abort

function:
	to kill the child process and send 'abor' down the telnet
	command connection.

algorithm:
	kill child process
	make 'abort' into 'abor'
	write to the telnet connection

parameters:
	none

returns:
	nothing

globals:
	none

calls:
	kill (sys)
	net_write

called by:
	decodeanycommands ( thru funcarr )

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

abort()
{

	/* signal child to die */
	kill( childpid,9 );

	/* make 'abort' into 'abor' */
	termbytes--;

	/* send 'abor' on down the telnet command connection */
	net_write();
}

/*name:
	asciimode

function:
	send an ascii mode command to the foreign host
	set the asciimode bit.
	gets done.

algorithm:
	really

parameters:
	none

returns:
	global ascii set to 1

globals:
	ascii =
	termbytes=

calls:
	printf (sys)
	strmove
	net_write

called by:
	decodeanycomands thru funcarr 

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

asciimode()
{

	strmove("type a ",termbuf);
	termbytes = 7;
	net_write();
	ascii = 1;
}

/*name:
	binmode

function:
	send an image mode command to the foreign host
	reset the ascii flag
	gets done

algorithm:
	build a userftp 'type i' command
	send it to the network
	say we are in image mode
	set flag saying we are in image mode

parameters:
	none

returns:
	ascii set to 0

globals:
	ascii =
	termbuf=

calls:
	printf (sys)
	strmove
	net_write

called by:
	decodeanycommands thru funcarr

history:
	initial coding 6/9/75 by S. F. Holmgren


*/

binmode()
{
	strmove("type i ",termbuf);
	termbytes = 7;
	net_write();
	ascii = 0;
}

/*name:
	tenexmode

function:
	build and send a type l command to the foreign host
	reset the ascii mode flag

algorithm:
	build the type l command in termbuf
	set number of bytes in termbuf
	send it to the net with net_write
	reset the ascii mode flag

parameters:
	none

returns:
	foreign host in local data transfer mode

globals:
	termbuf=
	termbytes=

calls:
	strmove
	net_write

called by:
	decodeanycommands ( thru funcarr )

history:
	initial coding 10/6/75 by S. F. Holmgren

*/

tenexmode()
{
	strmove("type l ",termbuf);
	termbytes = 7;
	net_write();
	ascii = 0;
}
/*name:
	done		-		called in response to bye or end

function:
	to signal foreign host that we are leaving
	to signal main to exit

algorithm:
	build userftp 'bye' command
	send it off to network
	set weshould to zero

parameters:
	none

returns:
	weshould set to zero

globals:
	termbytes=
	weshould=

calls:
	strmove
	net_write

called by:
	decodeanycommands thru funcarr

history:
	initial coding 7/1/75 by S. F. Holmgren

*/

done()
{

	strmove("bye ",termbuf);	/* load in/out buff with ftp by comm */
	termbytes = 4;			/* say there is some data */
	net_write();			/* tell foreign host were going */
	weshould = 0;		/* tell main loop to bag it */
}

/*name:
	log		-	translates usercode password into ftp user
				pass commands

function:
	see name

algorithm:
	get usercode
	send to net
	set tty to no echo
	get password
	send to net
	set tty to echo
	get account
	if none return
	if any, send to net

parameters:
	none

returns:
	user, pass and possibly acct commands sent to net

globals:
	termbuf=
	termbytes=

calls:
	strmove
	net_write
	getnullstr

called by:
	main
	decodeanycommands thru funcarr

history:
	initial coding 7/1/75 by S. F. Holmgren

*/

log()
{
	register savetrmbytes;
	printf("Username:");		/* get user name */
	strmove("user ",termbuf);	/* formulate piece of user comm */
	getnullstr(5);			/* formulate rest of user comm */
	/* stick crlf on the end of string and set up for more from term */
	termbuf[ termbytes-1 ] = CR;
	termbuf[ termbytes ] = LF;
	termbytes++;

	gtty(0,tty);		/* get terminal options */
	tty[2] =& ~ECHO;		/* reset echo */
	stty( 0,tty );		/* do it to terminal */

	printf("Password:");		/* ask for password */
	strmove("pass ",&termbuf[ termbytes ]);	/* form piece of pass command */
	getnullstr(5+termbytes);			/* form rest of pass command */
	savetrmbytes = termbytes;
	termbuf[termbytes-1] = CR;
	termbuf[termbytes] = LF;
	termbytes++;

	gtty( 0,tty );		/* get term options */
	tty[2] =| ECHO;		/* start echoing again */
	stty( 0,tty );		/* do it */
	printf("\n");			/* echo cr after password */

	printf("Account:");		/* ask for his account */
	strmove("acct ",&termbuf[ termbytes ]);	/* form start of acct command */
	getnullstr(5+termbytes);			/* get the guy's account */
	if( termbytes == 7+savetrmbytes )	/* if the guy doesnt want one */
		termbytes = savetrmbytes;
	net_write();			/* send it if he does */
}

/*name
	term_write

function:
	if there is any data from the net take it and print
	it on the controlling teletype

algorithm:
	see function

parameters:
	termbytes
	termbuf (global)

returns:
	some data on the users terminal

globals:
	termbuf
	termbytes

calls:
	crlf_to_lf
	write (sys)

called by:
	main

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

term_write()
{

		/* translate crlf -> lf */
		crlf_to_lf(1);
}

/*name:
	net_read

function:
	to check if there is data to read from the net
	if so read it into termbuf and set termbytes
	otherwise
	set termbytes to zero

algorithm:
	see function

parameters:
	netfile

returns:
	data into termbuf (global)

globals:
	termbytes=
	termbuf=

calls:
	gtty (sys)
	read (sys)

called by:
	main

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

net_read()
{

		/* get data from net */
		if ( ( termbytes = read( netfid,termbuf,termsize )) <= 0 )
			/* if there were errors then exit */
			exit();
}

/*name:
	strmove

function:
	copy a null terminated string into a vector 

algorithm:
	while there is data copy the data

parameters:
	src	-	char pointer to a null termed string
	dest	-	char pointer to a place to stick the string

returns:
	pointer to the null at the end of the destination string

globals:
	none

calls:
	nothing

called by:
	connftpserver
	get
	put

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

strmove( src,dest )
char *src;
char *dest;
{

	register char *srcp;	/* for speed */
	register char *destp;	/* for speed */

	srcp = src;
	destp = dest;

	/* copy str including null at the end */
	while( (*destp = *srcp++) ) destp++;

	/* return pointer to the null at the end */
	return( destp );
}

/*name:
	getnullstr

function:
	to read a string in from the terminal into termbuf 
	and stick a null on the end.
	the index says where to read the data in relative to the
	beginning of termbuf.  That index is also reflected in
	determining a new value for termbytes

algorithm:
	call term_read to get data from terminal into right place
	stick a null over the last character in the string,
	( replacing the LF that is appened to each stnd read from term )

parameters:
	index	-	location within termbuf to start putting chars

returns:
	a null terminated string of input from the terminal in termbuf
	with termbytes set to the right number of bytes

globals:
	termbuf=
	termbytes=

calls:
	term_read

called by:
	main
	get
	put

history:
	initial coding 6/9/75 by S. F. Holmgren
	modified to call term_read 9/27/76 by S. F. Holmgren

*/

getnullstr( index )
{

	/* call term_read to get chars into right place */
	term_read( index );
	/* make into null termed string */
	termbuf[ termbytes-1 ] = 0;
}

/*name:
	compare

function:
	compare two strings for the length of the first for sameness

algorithm:
	while there is data to compare
		if they dont compare return failure
	return success

parameters:
	null terminated comparator string
	comparator candidate

returns:
	0 if didnt compare
	1 if compared

globals:
	none

calls:
	nothing

called by:
	whichcomm

history:
	initial coding 6/9/75 by S. F. Holmgren

*/

compare( str1,str2 )
char *str1;
char *str2;
{

	register char *str1p;	/* for speed */
	register char *str2p;	/* for speed */

	str1p = str1;
	str2p = str2;

	/* while there is data in str1 */
	while( *str1p )
		if( *str1p++ != *str2p++ )	/* if they dont compare */
			return( 0 );
	/* got to here must have all compared - return success */
	return( 1 );
}

setsignals()
{
	extern int (*sayerr)();
	extern int (*statistics)();

	signal(4,&sayerr);
	signal(5,&sayerr);
	signal(6,&sayerr);
	signal(7,&sayerr);
	signal(8,&sayerr);
	signal(10,&sayerr);
	signal(11,&sayerr);
	signal(12,&sayerr);
	signal(13,&sayerr);
}

sayerr()
{
	perror("**** FTP Internal Error ****");
}

statistics()
{
	long net_time;

	/* say how many bytes transferred and ultimately what baud rate */
	if (!gottime) {
		printf ("Sorry, no data transferred\n");
		return;
	}
	printf("%L bytes transfered", bytestransferred);
	net_time = stoptime - starttime;
	printf(" over a period of %L seconds",net_time);
	if (!net_time) net_time = 1; /* no divide's by zero, plz */
	printf(" (%L baud)\n",
		(bytestransferred<<3)/net_time );
}
/*name:
	unixcommand

function:
	to fork an inferior shell and execute a command which the
	user gives while in ftp.  Such commands are recognized by
	the fact that the first character of the line is an '!'
	(exclamation point).

algorithm:
	Null terminate the string in termbuf, do a fork, and have
	the new guy exec a shell (passing it a "-c" and a pointer
	to the shell command in termbuf).  The old guy will wait
	until the new guy exits.

parameters:
	None, its only "argument" is found in termbuf.

returns:
	a perfunctory 0

globals:
	termbuf	- where the command to be executed is
	termbytes - to know how many characters are in line

calls:
	fork	- to fork the shell
	execl	- to pass its arguments
	wait	- to await the death of the inferior shell
	signal	- to ignore interrupts (and restore them later)

called by:
	decodeanycommands ( thru funcarr )

history:
	initial coding 9/30/75 by M. Kampe

*/

unixcommand()
{
	register int i;
	register int j;
	int waitstatus;

	termbuf[termbytes-1] = '\000';
	i = fork();
	if (i<0) return(0);
	if (i)
	{	j = signal(2,1);
		while( wait(&waitstatus) != i);
		signal(2,j);
		printf("!\n");
		return(0);
	}
	else
	{	signal(2,0);
		execl("/bin/sh","sh","-c",&termbuf[1], 0);
		exit(0);
	}
}
  