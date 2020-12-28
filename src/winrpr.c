/*
 * Support for SCS WINRPR (tcp/ip control) as a digital modem
 *
 * This is basically my 2012 AGWPE driver with 'stuff' stripped out.
 *
 * Prototype Version - November 2, 2020, by Maiko Langelaar, VE4KLM
 *
 * (C)opyright 2020 Maiko Langelaar, VE4KLM
 *
 * For Amateur Radio use only (please) !
 */

#include "global.h"

#ifdef	WINRPR

#include "mbuf.h"
#include "iface.h"
#include "slip.h"
#include "trace.h"
#include "pktdrvr.h"
#include "devparam.h"

/*
 * 29Sep2019, Maiko (VE4KLM), struct breakouts
 */
#include "ax25.h"
#include "netuser.h"

#define UNUSED(x) ((void)(x))   /* 15Apr2016, VE4KLM, trick to suppress any
                                 * unused variable warning, why are there so
                                 * many MACROs in this code, should convert
                                 * them to actual functions at some point. */
typedef struct agwpecparm {
	char *hostname;
	int portnum;
} IFPAGWPE;

/* 27Feb2009, Maiko, Use an existing function from slip.c module  */
extern struct mbuf *slip_decode (register struct slip *sp, char c);

/* 28Feb2009, Maiko, Use an existing function from slip.c module  */
extern struct mbuf *slip_encode (struct mbuf *bp, int usecrc);

static int psock = -1;

static int connectwinrpr (IFPAGWPE *ifpp)
{
	static int logcount = 0, logstage = 1;

    struct sockaddr_in fsocket;

    int s = -1;
  
	if ((fsocket.sin_addr.s_addr = resolve (ifpp->hostname)) == 0L)
	{
		log (-1, "winrpr - host (%s) not found", ifpp->hostname);
        return -1;
    }
  
    fsocket.sin_family = AF_INET;
    fsocket.sin_port = ifpp->portnum;
 
	if ((s = j2socket (AF_INET, SOCK_STREAM, 0)) == -1)
	{
		log (-1, "winrpr - no socket");
        return -1;
	}

	sockmode (s, SOCK_BINARY);
  
	if (j2connect (s, (char*)&fsocket, SOCKSIZE) == -1)
	{

		/*
		 * 26Nov2020, Maiko (VE4KLM), an idea to back off on the number of
		 * log entries created if the device is offline for long periods.
		 */
		if (!(logcount % logstage))
		{
			log (-1, "winrpr - connect failed, errno %d", errno);
			logstage *= 2;
            logcount = 0;
        }

		logcount++;

        return -1;
	}
  
	log (s, "winrpr [%s] - connected", ifpp->hostname);

	/*
	 * 26Nov2020, Maiko (VE4KLM), reset on connect, or we may not
	 * see subsequent connect failures in the logs for some time.
	 */
	logcount = 0;
	logstage = 1;

	return s;
}

int winrpr_send (struct iface *iface, struct mbuf *bp)
{
	static char *iobuf = NULL;
	static int iomtu = -1;
	struct mbuf *tbp;
	char *ptr;
	int len;

	if (iomtu == -1 || iface->mtu > iomtu)
	{
		if (iomtu != -1)
			free (iobuf);

		iomtu = iface->mtu;
		iobuf = mallocw (iomtu + 3);	/* fend and ctrl as well */
	}

	if (psock != -1)	/* tcp/ip socket handle */
	{
		len = len_p (bp);

		ptr = iobuf;

		/* this should never happen, but just to be safe !!! */
		if (len > iomtu)
			log (-1, "winrpr_send - packet (%d) larger then mtu", len);

		else if (dup_p (&tbp, bp, 0, len) != len)
			log (-1, "winrpr_send - dup_p failed");

		else
		{
			*ptr++ = 0xc0;	/* 10Nov2020, from the KISS TNC Spec */

			*ptr++ = 0x00;	/* sending out data on port 0 */

			pullup (&tbp, ptr, len);

			ptr += len;
 		
			/*
			 * 28Dec2020, Maiko, I am an idiot ! This explains the
			 * inconsistent transmit behaviour I've been seeing for
			 * the past couple month - completely forgot about the
			 * trailing FEND, and now it's consistent, finally got
			 * my PTT (rts) working as well, sorry folks ...
			 *
			 * Also, originally though KISS ctrl codes would be
			 * handled since I thought I was using SLIP routines,
			 * but no, so expect a bit of a rewrite 'soon', binary
			 * data may not work very well, just a heads up :]
			 */

			*ptr++ = 0xc0;	/* trailing FEND */

			if (j2send (psock, iobuf, len + 3, 0) < 1)
				log (-1, "winrpr_send - write errno %d", errno);

			free_p (tbp);
		}

	    free_p (bp);
	}

    return 0;
}

int winrpr_raw (struct iface *iface, struct mbuf *bp)
{
	dump (iface, IF_TRACE_OUT, CL_AX25, bp);

    iface->rawsndcnt++;
    iface->lastsent = secclock();
  
	winrpr_send (iface, bp);

    return 0;
}

void winrpr_rx (int xdev, void *p1, void *p2)
{
	struct iface *ifp = (struct iface*)p1;
	IFPAGWPE *ifpp = (IFPAGWPE*)p2;
    struct sockaddr_in from;
    struct mbuf *bp;
	char tmp[AXBUF];
	int fromlen;

	log (-1, "winrpr listener [%s:%d]", ifpp->hostname, ifpp->portnum);

	while (1)
	{
		/* Connect to the winrpr tcp/ip server, retry every minute */

		if (psock == -1)
		{
			while ((psock = connectwinrpr (ifpp)) == -1)
				j2pause (60000);
		}

		/* Handle incoming data stream */

		while (1)
		{
			// pwait (NULL);	/* give other processes a chance */

			if (recv_mbuf (psock, &bp, 0, (char*)&from, &fromlen) == -1)
				break;

			pullup (&bp, tmp, 2);	/* 10Nov2020, FEND and data frame port 0 */

			/* NICE - just pass the rest of BP direct to net_route :) */

			if (net_route (ifp, CL_AX25,  bp) != 0)
			{
				free_p (bp);
				bp = NULL;
			}
		}

		log (-1, "winrpr disconnected");
		close_s (psock);
		psock = -1;
	}
}

int winrpr_attach (int argc, char *argv[], void *p)
{
	struct iface *ifp;
	struct slip *sp;
	IFPAGWPE *ifpp;
	int xdev;
 
	if (if_lookup (argv[1]) != NULLIF)
	{
		tprintf (Existingiface, argv[4]);
		return -1;
	}

	/* Create structure of AGWPE connection parameters */

	ifpp = (IFPAGWPE*)callocw (1, sizeof(IFPAGWPE));

	ifpp->hostname = j2strdup (argv[2]);
	ifpp->portnum = atoi (argv[3]);

	/* Create interface structure and fill in details */

	ifp = (struct iface*)callocw (1, sizeof(struct iface));

	ifp->addr = Ip_addr;
	ifp->name = j2strdup (argv[1]);
	ifp->mtu = 256;
	ifp->dev = 0;
	ifp->stop = NULL;

	if (ifp->hwaddr == NULLCHAR)
		ifp->hwaddr = mallocw (AXALEN);

	memcpy (ifp->hwaddr, Mycall, AXALEN);

	setencap (ifp, "AX25");

	ifp->ioctl = NULL;
	ifp->raw = winrpr_raw;
	ifp->show = NULL;
	ifp->flags = 0;

	/*
	 * 28Feb2009, Maiko, Crap, it looks like the Slip[] and xdev and dev are
	 * so interlocked into the trace (dump) routines, that dump will actually
	 * crash (and probably other stuff) if I just calloc a SLIP structure, as
	 * I tried to do before. Looks like I'll have to stick with looping thru
	 * the Slip[] array for an empty slot, until I figure out another way to
	 * do this. It works ! BUT I don't like how it's tied into everything.
	 */
	for (xdev = 0; xdev < SLIP_MAX; xdev++)
	{
		sp = &Slip[xdev];
		if (sp->iface == NULLIF)
			break;
	}
	if (xdev >= SLIP_MAX)
	{
		j2tputs ("Too many slip devices\n");
		return -1;
	}

	ifp->xdev = xdev;
	sp->iface = ifp;
	sp->kiss[ifp->port] = ifp;
	sp->type = CL_KISS;
	sp->polled = 0;
	sp->usecrc = 0;

	/* Link in the interface - important part !!! */
	ifp->next = Ifaces;
	Ifaces = ifp;

	/*
	 * Create a listener process
	 *
	 * The connect to the winrpr tcp/ip server is inside the
	 * listener process. If we do it here, JNOS will hang on the
	 * attach command - remember 'attach' must always return.
	 */
	ifp->rxproc = newproc ("winrpr_rx", 1024, winrpr_rx,
			0, (void*)ifp, (void*)ifpp, 0);

	return 0;
}

#endif	/* End of WINRPR */
