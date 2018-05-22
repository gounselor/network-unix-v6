#
/*
 */

/*
 * general TTY subroutines
 */
#include "param.h"
#include "systm.h"
#include "user.h"
#include "tty.h"
#include "proc.h"
#include "inode.h"
#include "file.h"
#include "reg.h"
#include "conf.h"

/*
 * Input mapping table-- if an entry is non-zero, when the
 * corresponding character is typed preceded by "\" the escape
 * sequence is replaced by the table value.  Mostly used for
 * upper-case only terminals.
 */
char	maptab[]
{
	000,000,000,000,004,000,000,000,
	000,000,000,000,000,000,000,000,
	000,000,000,000,000,000,000,000,
	000,000,000,000,000,000,000,000,
	000,'|',000,'#',000,000,000,'`',
	'{','}',000,000,000,000,000,000,
	000,000,000,000,000,000,000,000,
	000,000,000,000,000,000,000,000,
	'@',000,000,000,000,000,000,000,
	000,000,000,000,000,000,000,000,
	000,000,000,000,000,000,000,000,
	000,000,000,000,000,000,'~',000,
	000,'A','B','C','D','E','F','G',
	'H','I','J','K','L','M','N','O',
	'P','Q','R','S','T','U','V','W',
	'X','Y','Z',000,000,000,000,000,
};

/*
 * The actual structure of a clist block manipulated by
 * getc and putc (mch.s)
 */
struct cblock {
	struct cblock *c_next;
	char info[6];
};

#ifndef UCBUFMOD
/* The character lists-- space for 6*NCLIST characters */
struct cblock cfree[NCLIST];
#endif not UCBUFMOD
#ifdef UCBUFMOD
/* pointer to the external clist area */
int clistp;
#endif UCBUFMOD
/* List head for unused character blocks. */
struct cblock *cfreelist;

/*
 * structure of device registers for KL, DL, and DC
 * interfaces-- more particularly, those for which the
 * SSTART bit is off and can be treated by general routines
 * (that is, not DH).
 */
struct {
	int ttrcsr;
	int ttrbuf;
	int tttcsr;
	int tttbuf;
};

/*
 * routine called on first teletype open.
 * establishes a process group for distribution
 * of quits and interrupts from the tty.
 */
ttyopen(dev, atp)
struct tty *atp;
{
	register struct proc *pp;
	register struct tty *tp;

	pp = u.u_procp;
	tp = atp;
	if(pp->p_pgrp == 0) {
		pp->p_pgrp = pp->p_pid;
		u.u_ttyp = tp;
		u.u_ttyd = dev;
		tp->t_pgrp = pp->p_pid;
	}
	tp->t_state =& ~WOPEN;
	tp->t_state =| ISOPEN;
}

/*
 * The routine implementing the gtty system call.
 * Just call lower level routine and pass back values.
 */
gtty()
{
	int v[3];
	register *up, *vp;

	vp = v;
	sgtty(vp);
	if (u.u_error)
		return;
	up = u.u_arg[0];
	suword(up, *vp++);
	suword(++up, *vp++);
	suword(++up, *vp++);
}

/*
 * The routine implementing the stty system call.
 * Read in values and call lower level.
 */
stty()
{
	register int *up;

	up = u.u_arg[0];
	u.u_arg[0] = fuword(up);
	u.u_arg[1] = fuword(++up);
	u.u_arg[2] = fuword(++up);
	sgtty(0);
}

/*
 * Stuff common to stty and gtty.
 * Check legality and switch out to individual
 * device routine.
 * v  is 0 for stty; the parameters are taken from u.u_arg[].
 * v  is non-zero for gtty and is the place in which the device
 * routines place their information.
 */
sgtty(v)
int *v;
{
	register struct file *fp;
	register struct inode *ip;

	if ((fp = getf(u.u_ar0[R0])) == NULL)
		return;
	ip = fp->f_inode;
	if ((ip->i_mode&IFMT) != IFCHR) {
		u.u_error = ENOTTY;
		return;
	}
	(*cdevsw[ip->i_addr[0].d_major].d_sgtty)(ip->i_addr[0], v);
}

/*
 * Wait for output to drain, then flush input waiting.
 */
wflushtty(atp)
struct tty *atp;
{
	register struct tty *tp;

	tp = atp;
	spl5();
	while (tp->t_outq.c_cc) {
		tp->t_state =| ASLEEP;
		sleep(&tp->t_outq, TTOPRI);
	}
	flushtty(tp);
	spl0();
}

/*
 * Initialize clist by freeing all character blocks, then count
 * number of character devices. (Once-only routine)
 */
cinit()
{
	register int ccp;
	register struct cblock *cp;
	register struct cdevsw *cdp;

#ifndef UCBUFMOD
	ccp = cfree;
	for (cp=(ccp+07)&~07; cp <= &cfree[NCLIST-1]; cp++) {
		cp->c_next = cfreelist;
		cfreelist = cp;
	}
#endif not UCBUFMOD
#ifdef UCBUFMOD
	clistp = malloc(coremap, (NCLIST+7)/8);
	cfreelist = m_cinit((NCLIST+7)/8*8);
#endif UCBUFMOD
#ifndef NOSC	/* nchrdev is included in c.c */
	ccp = 0;
	for(cdp = cdevsw; cdp->d_open; cdp++)
		ccp++;
	nchrdev = ccp;
#endif not NOSC
}

/*
 * flush all TTY queues
 */
flushtty(atp)
struct tty *atp;
{
	register struct tty *tp;
	register int sps;

	tp = atp;
	while (getc(&tp->t_canq) >= 0);
	while (getc(&tp->t_outq) >= 0);
	wakeup(&tp->t_rawq);
	wakeup(&tp->t_outq);
	sps = PS->integ;
	spl5();
	while (getc(&tp->t_rawq) >= 0);
	tp->t_delct = 0;
	PS->integ = sps;
}

/*
 * transfer raw input list to canonical list,
 * doing erase-kill processing and handling escapes.
 */
#ifdef CANBSIZ
/*
 * It waits until a full line has been typed in cooked mode,
 * or until any character has been typed in raw mode.
 */
char canonb[CANBSIZ];

canon(atp)
struct tty *atp;
{
	register char *bp;
	char *bp1;
	register struct tty *tp;
	register int c;

	tp = atp;
	spl5();
	while (tp->t_delct==0) {
		if ((tp->t_state&CARR_ON)==0)
			return(0);
		sleep(&tp->t_rawq, TTIPRI);
	}
	spl0();
loop:
	bp = &canonb[2];
	while ((c=getc(&tp->t_rawq)) >= 0) {
		if (c==0377) {
			tp->t_delct--;
			break;
		}
		if ((tp->t_flags&RAW)==0) {
			if (bp[-1]!='\\') {
				if (c==tp->t_erase) {
					if (bp > &canonb[2])
						bp--;
					continue;
				}
				if (c==tp->t_kill)
					goto loop;
				if (c==CEOT)
					continue;
			} else
			if (maptab[c] && (maptab[c]==c || (tp->t_flags&LCASE))) {
				if (bp[-2] != '\\')
					c = maptab[c];
				bp--;
			}
		}
		*bp++ = c;
		if (bp>=canonb+CANBSIZ)
			break;
	}
	bp1 = bp;
	bp = &canonb[2];
	c = &tp->t_canq;
	while (bp<bp1)
		putc(*bp++, c);
	return(1);
}

/*
 * Place a character on raw TTY input queue, putting in delimiters
 * and waking up top half as needed.
 * Also echo if required.
 * The arguments are the character and the appropriate
 * tty structure.
 */
ttyinput(ac, atp)
struct tty *atp;
{
	register int t_flags, c;
	register struct tty *tp;

	tp = atp;
	c = ac;
	t_flags = tp->t_flags;
	if ((c =& 0177) == '\r' && t_flags&CRMOD)
		c = '\n';
	if ((t_flags&RAW)==0 && (c==CQUIT || c==CINTR)) {
		signal(tp, c==CINTR? SIGINT:SIGQIT);
		flushtty(tp);
		return;
	}
	if (tp->t_rawq.c_cc>=TTYHOG) {
		flushtty(tp);
		return;
	}
	if (t_flags&LCASE && c>='A' && c<='Z')
		c =+ 'a'-'A';
	putc(c, &tp->t_rawq);
	if (t_flags&RAW || c=='\n' || c==004) {
		wakeup(&tp->t_rawq);
		if (putc(0377, &tp->t_rawq)==0)
			tp->t_delct++;
	}
	if (t_flags&ECHO) {
		ttyoutput(c, tp);
		ttstart(tp);
	}
}
#endif CANBSIZ

#ifndef CANBSIZ
/*
 * Asynchronous echoing.  Typeahead is accumulated in the raw queue and
 * canonicalized on the fly.
 */
canon(atp)
struct tty *atp;
{
	register struct clist *q;
	register struct tty *tp;
	register int c;

	tp = atp;  q = &tp->t_canq;
	do {
		if( (c=ttchar(tp)) < 0 ) return (0);	/* get char */
		if( c == '\\' ) {	/* literal escape */
			if( (c=ttchar(tp)) < 0) return (0);	/* get escaped char */
			if(maptab[c] && (maptab[c] == c || (tp->t_flags&LCASE)))
				putc(maptab[c], q);	/* unmap chars mapped by input routine */
				/* as a side effect, removes quote before erase, kill,and EOT */
			else {	/* leave escape sequence for somebody later */
				putc('\\', q);
				putc(c, q);
			}
		} else {		/* not an escape sequence */
			if(c == tp->t_erase)	/* if erase char */
				backc(q);		/* back it off */
			else if(c == tp->t_kill) /* if kill char */
				while(getc(q) >= 0);	/* zap line */
			else if(c == CEOT)	/* if an EOT */
				break;			/* send what we've got */
			else putc(c, q);	/* otherwise, just copy char */
		}
	} while (c != '\n');	/* continue scan until EOL */
	ttstart(tp);	/* make sure tty started (queue may not be dead) */
	return(1);
}

/*
 * Returns next character from input stream, echoed as necessary.
 */
char ttchar(tp)
struct tty *tp;
{
	register char c;

	spl5();		/* test and sleep must be indivisible */
	while(tp->t_rawq.c_cc == 0) {	/* any chars in raw queue? */
		if((tp->t_state&CARR_ON) == 0)	/* die if lost line */
			return -1;
		ttstart(tp);	/* start any output we may have echoed */
		tp->t_state =| NOTIFY;	/* tell interrupt to let us know */
		sleep(&tp->t_rawq, TTIPRI);	/* wait until chars arrive */
		tp->t_state =& ~NOTIFY;
	}
	spl0();
	if( (c = getc(&tp->t_rawq)) & 0200 )	/* already echoed? */
		c =& 0177;		/* if so, extract echoed character */
	else if(tp->t_flags&ECHO) {	/* echo character as necessary */
		ttyoutput(c, tp);
	}
	return(c);		/* return character */
}

/*
 * Place a character on raw TTY input queue, waking up 
 * input process as required.
 * The arguments are the character and the appropriate
 * tty structure.
 */
ttyinput(ac, atp)
struct tty *atp;
{
	register int t_flags, c;
	register struct tty *tp;

	tp = atp;
	c = ac;
	t_flags = tp->t_flags;
	if ((c =& 0177) == '\r' && t_flags&CRMOD)
		c = '\n';
	if (t_flags&LCASE && c>='A' && c<='Z')
		c =+ 'a'-'A';
	if ((t_flags&RAW)==0 && (c==CQUIT || c==CINTR)) {
		signal(tp->t_pgrp, c==CINTR? SIGINT:SIGQIT);
		flushtty(tp);
		return;
	}
	if((tp->t_state&NOTIFY) == 0) {		/* Is this typeahead? */
		if(tp->t_rawq.c_cc >= TTYHOG) {		/* Yes; too much? */
			ttyoutput('\007', tp); /* bell */
			ttstart(tp);
			return;
		}
	} else {	/* Otherwise, process is awaiting input */
		if(t_flags&RAW ||		/* Raw mode... */
		    c == '\n' || c == CEOT ||	/* Normal mode EOL... */
		    tp->t_rawq.c_cc >= TTYHOG)	/* A bunch of input... */
			wakeup(&tp->t_rawq);	/* Let process handle it */
		if(t_flags&ECHO) {
			ttyoutput(c, tp);	/* Interrupt time echo */
			ttstart(tp);
			c =| 0200;
		}
	}
	putc(c, &tp->t_rawq);
}
#endif not CANBSIZ

/*
 * put character on TTY output queue, adding delays,
 * expanding tabs, and handling the CR/NL bit.
 * It is called both from the top half for output, and from
 * interrupt level for echoing.
 * The arguments are the character and the tty structure.
 */
ttyoutput(ac, tp)
struct tty *tp;
{
	register int c;
	register struct tty *rtp;
	register char *colp;
	int ctype;

	rtp = tp;
	c = ac&0177;
	/*
	 * Ignore EOT in normal mode to avoid hanging up
	 * certain terminals.
	 */
	if (c==CEOT && (rtp->t_flags&RAW)==0)
		return;
	/*
	 * Turn tabs to spaces as required
	 */
	if (c=='\t' && rtp->t_flags&XTABS) {
		do
			ttyoutput(' ', rtp);
		while (rtp->t_col&07);
		return;
	}
	/*
	 * for upper-case-only terminals,
	 * generate escapes.
	 */
	if (rtp->t_flags&LCASE) {
		colp = "({)}!|^~'`";
		while(*colp++)
			if(c == *colp++) {
				ttyoutput('\\', rtp);
				c = colp[-2];
				break;
			}
		if ('a'<=c && c<='z')
			c =+ 'A' - 'a';
	}
	/*
	 * turn <nl> to <cr><lf> if desired.
	 */
	if (c=='\n' && rtp->t_flags&CRMOD)
		ttyoutput('\r', rtp);
	if (putc(c, &rtp->t_outq))
		return;
	/*
	 * Calculate delays.
	 * The numbers here represent clock ticks
	 * and are not necessarily optimal for all terminals.
	 * The delays are indicated by characters above 0200,
	 * thus (unfortunately) restricting the transmission
	 * path to 7 bits.
	 */
	colp = &rtp->t_col;
	ctype = partab[c];
	c = 0;
	switch (ctype&077) {

	/* ordinary */
	case 0:
		(*colp)++;

	/* non-printing */
	case 1:
		break;

	/* backspace */
	case 2:
		if (*colp)
			(*colp)--;
		break;

	/* newline */
	case 3:
		ctype = (rtp->t_flags >> 8) & 03;
		if(ctype == 1) { /* tty 37 */
			if (*colp)
				c = max((*colp>>4) + 3, 6);
		} else
		if(ctype == 2) { /* vt05 */
			c = 6;
		} else if(ctype == 3) { /* Omron, Tektronix 4023 */
			ttyoutput(0 /* null */, rtp);
			ttyoutput(0 /* null */, rtp);
			ttyoutput(0 /* null */, rtp);
			ttyoutput(0 /* null */, rtp);
		}
		*colp = 0;
		break;

	/* tab */
	case 4:
		ctype = (rtp->t_flags >> 10) & 03;
		if(ctype == 1) { /* tty 37 */
			c = 1 - (*colp | ~07);
			if(c < 5)
				c = 0;
		}
		*colp =| 07;
		(*colp)++;
		break;

	/* vertical motion */
	case 5:
		if(rtp->t_flags & VTDELAY) /* tty 37 */
			c = 0177;
		break;

	/* carriage return */
	case 6:
		ctype = (rtp->t_flags >> 12) & 03;
		if(ctype == 1) { /* tn 300 */
			c = 5;
		} else
		if(ctype == 2) { /* ti 700 */
			c = 10;
		}
		*colp = 0;
	}
	if(c)
		putc(c|0200, &rtp->t_outq);
}

/*
 * Restart typewriter output following a delay
 * timeout.
 * The name of the routine is passed to the timeout
 * subroutine and it is called during a clock interrupt.
 */
ttrstrt(atp)
{
	register struct tty *tp;

	tp = atp;
	tp->t_state =& ~TIMEOUT;
	ttstart(tp);
}

/*
 * Start output on the typewriter. It is used from the top half
 * after some characters have been put on the output queue,
 * from the interrupt routine to transmit the next
 * character, and after a timeout has finished.
 * If the SSTART bit is off for the tty the work is done here,
 * using the protocol of the single-line interfaces (KL, DL, DC);
 * otherwise the address word of the tty structure is
 * taken to be the name of the device-dependent startup routine.
 *
 * Bug fix  UNIX NEWS Vol 2, No 8, Sept 1977, P8-4
 * R. Sidebotham - University of Calgary
 *
 */
ttstart(atp)
struct tty *atp;
{
	register int *addr, c;
	register struct tty *tp;
	int sps;
	struct { int (*func)(); };

	tp = atp;
	addr = tp->t_addr;
	if (tp->t_state&SSTART) {
		(*addr.func)(tp);
		return;
	}
	sps = PS->integ;        /* bug fix */
	spl5();                 /* bug fix */
	if ((addr->tttcsr&DONE)==0 || tp->t_state&TIMEOUT)
		return;
	if ((c=getc(&tp->t_outq)) >= 0) {
		if (c<=0177)
			addr->tttbuf = c | (partab[c]&0200);
		else {
			timeout(ttrstrt, tp, c&0177);
			tp->t_state =| TIMEOUT;
		}
	}
	PS->integ = sps;        /* bug fix */
}

/*
 * Called from device's read routine after it has
 * calculated the tty-structure given as argument.
 * The pc is backed up for the duration of this call.
 * In case of a caught interrupt, an RTI will re-execute.
 */
ttread(atp)
struct tty *atp;
{
	register struct tty *tp;
#ifndef CANBSIZ
	register char c;
#endif not CANBSIZ

	tp = atp;
	if ((tp->t_state&CARR_ON)==0)
		return;
#ifdef CANBSIZ
	if (tp->t_canq.c_cc || canon(tp))
		while (tp->t_canq.c_cc && passc(getc(&tp->t_canq))>=0);
#endif CANBSIZ
#ifndef CANBSIZ
	if (tp->t_canq.c_cc == 0) {	/* if nothing pre-cooked */
		if (tp->t_flags&RAW) {	/* he likes his raw */
			do {		/* so we will feed it to him raw */
				if( (c=ttchar(tp)) < 0) return;
			} while(passc(c) >= 0 && tp->t_rawq.c_cc &&
				c != '\n' && c != CEOT);
			ttstart(tp);
			return;
		}		/* he likes it cooked */
		if(canon(tp) == 0) return;	/* cook a line */
	}
		/* feed it to him */
	while(tp->t_canq.c_cc && passc(getc(&tp->t_canq)) >= 0);
#endif not CANBSIZ
}

/*
 * Called from the device's write routine after it has
 * calculated the tty-structure given as argument.
 */
ttwrite(atp)
struct tty *atp;
{
	register struct tty *tp;
	register int c;

	tp = atp;
	if ((tp->t_state&CARR_ON)==0)
		return;
	while ((c=cpass())>=0) {
		spl5();
		while (tp->t_outq.c_cc > TTHIWAT) {
			ttstart(tp);
			tp->t_state =| ASLEEP;
			sleep(&tp->t_outq, TTOPRI);
		}
		spl0();
		ttyoutput(c, tp);
	}
	ttstart(tp);
}

/*
 * Common code for gtty and stty functions on typewriters.
 * If v is non-zero then gtty is being done and information is
 * passed back therein;
 * if it is zero stty is being done and the input information is in the
 * u_arg array.
 */
ttystty(atp, av)
int *atp, *av;
{
	register  *tp, *v;

	tp = atp;
	if(v = av) {
		*v++ = tp->t_speeds;
		*v++ = tp->t_erase.integ;
		*v   = tp->t_flags;
#ifdef RAND
#ifdef CANBSIZ
		/* Temporary rand change for empty */
		u.u_arg[4] = (tp->t_canq.c_cc==0 &&
			tp->t_delct==0 && tp->t_state&CARR_ON);
#endif CANBSIZ
#ifndef CANBSIZ
		/* I honestly don't know what to do here! */
		!!!!! /* cause compile error */
#endif not CANBSIZ
#endif RAND
		return(1);
	}
#ifdef CANBSIZ
	wflushtty(tp);
#endif CANBSIZ
#ifndef CANBSIZ
	if(tp->t_erase.integ == u.u_arg[1]  &&  tp->t_flags == u.u_arg[2])
		wflushtty(tp);
#endif not CANBSIZ
	v = u.u_arg;
	tp->t_speeds = *v++;
	tp->t_erase.integ = *v++;
	tp->t_flags = *v;
	return(0);
}
