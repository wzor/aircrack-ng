/*
 *  pcap-compatible 802.11 packet sniffer
 *
 *  Copyright (C) 2006, 2007, 2008 Thomas d'Otreppe
 *  Copyright (C) 2004, 2005 Christophe Devine
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 *  In addition, as a special exception, the copyright holders give
 *  permission to link the code of portions of this program with the
 *  OpenSSL library under certain conditions as described in each
 *  individual source file, and distribute linked combinations
 *  including the two.
 *  You must obey the GNU General Public License in all respects
 *  for all of the code used other than OpenSSL. *  If you modify
 *  file(s) with this exception, you may extend this exception to your
 *  version of the file(s), but you are not obligated to do so. *  If you
 *  do not wish to do so, delete this exception statement from your
 *  version. *  If you delete this exception statement from all source
 *  files in the program, then also delete it here.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/time.h>

#ifndef TIOCGWINSZ
	#include <sys/termios.h>
#endif

#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <fcntl.h>


#include <sys/wait.h>

#include "version.h"
#include "pcap.h"
#include "uniqueiv.h"
//#include "crctable.h"
#include "crypto.h"

#include "osdep/osdep.h"

/* some constants */

#define ARPHRD_IEEE80211        801
#define ARPHRD_IEEE80211_PRISM  802
#define ARPHRD_IEEE80211_FULL   803

#define REFRESH_RATE 100000  /* default delay in us between updates */
#define DEFAULT_HOPFREQ 350  /* default delay in ms between channel hopping */

#define NB_PWR  5       /* size of signal power ring buffer */
#define NB_PRB 10       /* size of probed ESSID ring buffer */

#define MAX_CARDS 8	/* maximum number of cards to capture from */

#define	STD_OPN		0x0001
#define	STD_WEP		0x0002
#define	STD_WPA		0x0004
#define	STD_WPA2	0x0008

#define	ENC_WEP		0x0010
#define	ENC_TKIP	0x0020
#define	ENC_WRAP	0x0040
#define	ENC_CCMP	0x0080
#define ENC_WEP40	0x1000
#define	ENC_WEP104	0x0100

#define	AUTH_OPN	0x0200
#define	AUTH_PSK	0x0400
#define	AUTH_MGT	0x0800

#define	QLT_TIME	5
#define	QLT_COUNT	25

#ifdef MAX
#undef MAX
#endif
#define	MAX(a,b)	((a)>(b)?(a):(b))

//milliseconds to store last packets
#define BUFFER_TIME 3000

extern char * getVersion(char * progname, int maj, int min, int submin, int svnrev, int beta);
extern unsigned char * getmac(char * macAddress, int strict, unsigned char * mac);

const unsigned char llcnull[4] = {0, 0, 0, 0};
char *f_ext[4] = { "txt", "gps", "cap", "ivs" };

extern const unsigned long int crc_tbl[256];
extern const unsigned char crc_chop_tbl[256][4];

static uchar ZERO[32] =
"\x00\x00\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00";

int read_pkts=0;

int abg_chans [] =
{
    1, 7, 13, 2, 8, 3, 14, 9, 4, 10, 5, 11, 6, 12,
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108,
    112, 116, 120, 124, 128, 132, 136, 140, 149,
    153, 157, 161, 184, 188, 192, 196, 200, 204,
    208, 212, 216,0
};

int bg_chans  [] =
{
    1, 7, 13, 2, 8, 3, 14, 9, 4, 10, 5, 11, 6, 12, 0
};

int a_chans   [] =
{
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108,
    112, 116, 120, 124, 128, 132, 136, 140, 149,
    153, 157, 161, 184, 188, 192, 196, 200, 204,
    208, 212, 216,0
};

/* linked list of received packets for the last few seconds */
struct pkt_buf
{
    struct pkt_buf  *next;      /* next packet in list */
    unsigned char   *packet;    /* packet */
    unsigned short  length;     /* packet length */
    struct timeval  ctime;      /* capture time */
};

/* linked list of detected access points */

struct AP_info
{
    struct AP_info *prev;     /* prev. AP in list         */
    struct AP_info *next;     /* next  AP in list         */

    time_t tinit, tlast;      /* first and last time seen */

    int channel;              /* AP radio channel         */
    int max_speed;            /* AP maximum speed in Mb/s */
    int avg_power;            /* averaged signal power    */
    int power_index;          /* index in power ring buf. */
    int power_lvl[NB_PWR];    /* signal power ring buffer */
    int preamble;             /* 0 = long, 1 = short      */
    int security;             /* ENC_*, AUTH_*, STD_*     */
    int beacon_logged;        /* We need 1 beacon per AP  */
    int dict_started;         /* 1 if dict attack started */
    int ssid_length;          /* length of ssid           */

    unsigned long nb_bcn;     /* total number of beacons  */
    unsigned long nb_pkt;     /* total number of packets  */
    unsigned long nb_data;    /* number of  data packets  */
    unsigned long nb_data_old;/* number of data packets/sec*/
    int nb_dataps;  /* number of data packets/sec*/
    struct timeval tv;        /* time for data per second */

    unsigned char bssid[6];   /* the access point's MAC   */
    unsigned char essid[256]; /* ascii network identifier */

    unsigned char lanip[4];   /* last detected ip address */
                              /* if non-encrypted network */

    unsigned char **uiv_root; /* unique iv root structure */
                              /* if wep-encrypted network */

    int    rx_quality;        /* percent of captured beacons */
    int    fcapt;             /* amount of captured frames   */
    int    fmiss;             /* amount of missed frames     */
    unsigned int    last_seq; /* last sequence number        */
    struct timeval ftimef;    /* time of first frame         */
    struct timeval ftimel;    /* time of last frame          */
    struct timeval ftimer;    /* time of restart             */

    char *key;		      /* if wep-key found by dict */
    int essid_stored;         /* essid stored in ivs file? */

    char decloak_detect;      /* run decloak detection? */
    struct pkt_buf *packets;  /* list of captured packets (last few seconds) */
    char is_decloak;          /* detected decloak */

	// This feature eats 48Mb per AP
	int EAP_detected;
    unsigned char *data_root; /* first 2 bytes of data if */
    						  /* WEP network; used for    */
    						  /* detecting WEP cloak	  */
    						  /* + one byte to indicate   */
    						  /* (in)existence of the IV  */
};

struct WPA_hdsk
{
    uchar stmac[6];				 /* supplicant MAC               */
    uchar snonce[32];			 /* supplicant nonce             */
    uchar anonce[32];			 /* authenticator nonce          */
    uchar keymic[16];			 /* eapol frame MIC              */
    uchar eapol[256];			 /* eapol frame contents         */
    int eapol_size;				 /* eapol frame size             */
    int keyver;					 /* key version (TKIP / AES)     */
    int state;					 /* handshake completion         */
};

/* linked list of detected clients */

struct ST_info
{
    struct ST_info *prev;    /* the prev client in list   */
    struct ST_info *next;    /* the next client in list   */
    struct AP_info *base;    /* AP this client belongs to */
    time_t tinit, tlast;     /* first and last time seen  */
    unsigned long nb_pkt;    /* total number of packets   */
    unsigned char stmac[6];  /* the client's MAC address  */
    int probe_index;         /* probed ESSIDs ring index  */
    char probes[NB_PRB][256];/* probed ESSIDs ring buffer */
    int ssid_length[NB_PRB]; /* ssid lengths ring buffer  */
    int power;               /* last signal power         */
    int rate_to;             /* last bitrate to station   */
    int rate_from;           /* last bitrate from station */
    struct timeval ftimer;   /* time of restart           */
    int missed;              /* number of missed packets  */
    unsigned int lastseq;    /* last seen sequnce number  */
    struct WPA_hdsk wpa;     /* WPA handshake data        */
};

/* linked list of detected macs through ack, cts or rts frames */

struct NA_info
{
    struct NA_info *prev;    /* the prev client in list   */
    struct NA_info *next;    /* the next client in list   */
    time_t tinit, tlast;     /* first and last time seen  */
    unsigned char namac[6];  /* the stations MAC address  */
    int power;               /* last signal power         */
    int channel;             /* captured on channel       */
    int ack;                 /* number of ACK frames      */
    int ack_old;             /* old number of ACK frames  */
    int ackps;               /* number of ACK frames/s    */
    int cts;                 /* number of CTS frames      */
    int rts_r;               /* number of RTS frames (rx) */
    int rts_t;               /* number of RTS frames (tx) */
    int other;               /* number of other frames    */
    struct timeval tv;       /* time for ack per second   */
};
/* bunch of global stuff */

struct globals
{
    struct AP_info *ap_1st, *ap_end;
    struct ST_info *st_1st, *st_end;
    struct NA_info *na_1st, *na_end;

    unsigned char prev_bssid[6];
    unsigned char f_bssid[6];
    unsigned char f_netmask[6];
    char *dump_prefix;
    char *keyout;
    char *f_cap_name;

    int f_index;            /* outfiles index       */
    FILE *f_txt;            /* output csv file      */
    FILE *f_gps;            /* output gps file      */
    FILE *f_cap;            /* output cap file      */
    FILE *f_ivs;            /* output ivs file      */
    FILE *f_xor;            /* output prga file     */

    char * batt;            /* Battery string       */
    int channel[MAX_CARDS];           /* current channel #    */
    int ch_pipe[2];         /* current channel pipe */
    int cd_pipe[2];	    /* current card pipe    */
    int gc_pipe[2];         /* gps coordinates pipe */
    float gps_loc[5];       /* gps coordinates      */
    int save_gps;           /* keep gps file flag   */
    int usegpsd;            /* do we use GPSd?      */
    int * channels;
    int singlechan;         /* channel hopping set 1*/
    int chswitch;	    /* switching method     */
    int f_encrypt;          /* encryption filter    */
    int update_s;	    /* update delay in sec  */

    int is_wlanng[MAX_CARDS];          /* set if wlan-ng       */
    int is_orinoco[MAX_CARDS];         /* set if orinoco       */
    int is_madwifing[MAX_CARDS];       /* set if madwifi-ng    */
    int is_zd1211rw[MAX_CARDS];       /* set if zd1211rw    */
    int do_exit;            /* interrupt flag       */
    struct winsize ws;      /* console window size  */

    char * elapsed_time;	/* capture time			*/

    int one_beacon;         /* Record only 1 beacon?*/

    unsigned char sharedkey[3][512]; /* array for 3 packets with a size of \
                               up to 512Byte */
    time_t sk_start;
    char *prefix;
    int sk_len;

    int * own_channels;	    /* custom channel list  */

    int record_data;		/* do we record data?   */
    int asso_client;        /* only show associated clients */

    char * iwpriv;
    char * iwconfig;
    char * wlanctlng;
    char * wl;

    unsigned char wpa_bssid[6];   /* the wpa handshake bssid   */
    char message[512];
    char decloak;

    char is_berlin;           /* is the switch --berlin set? */
    int numaps;               /* number of APs on the current list */
    int maxnumaps;            /* maximum nubers of APs on the list */
    int maxaps;               /* number of all APs found */
    int berlin;               /* number of seconds it takes in berlin to fill the whole screen with APs*/
    /*
     * The name for this option may look quite strange, here is the story behind it:
     * During the CCC2007, 10 august 2007, we (hirte, Mister_X) went to visit Berlin
     * and couldn't resist to turn on airodump-ng to see how much access point we can
     * get during the trip from Finowfurt to Berlin. When we were in Berlin, the number
     * of AP increase really fast, so fast that it couldn't fit in a screen, even rotated;
     * the list was really huge (we have a picture of that). The 2 minutes timeout
     * (if the last packet seen is higher than 2 minutes, the AP isn't shown anymore)
     * wasn't enough, so we decided to create a new option to change that timeout.
     * We implemented this option in the highest tower (TV Tower) of Berlin, eating an ice.
     */

    int show_ack;
    int hide_known;

    int hopfreq;

    char*   s_file;         /* source file to read packets */
    char*   s_iface;        /* source interface to read from */
    FILE *f_cap_in;
    struct pcap_file_header pfh_in;
    int detect_anomaly;     /* Detect WIPS protecting WEP in action */
}
G;

int check_shared_key(unsigned char *h80211, int caplen)
{
    int m_bmac, m_smac, m_dmac, n, textlen;
    char ofn[1024];
    char text[256];
    char prga[512];
    unsigned int long crc;

    if((unsigned)caplen > sizeof(G.sharedkey[0])) return 1;

    m_bmac = 16;
    m_smac = 10;
    m_dmac = 4;

    if( time(NULL) - G.sk_start > 5)
    {
        /* timeout(5sec) - remove all packets, restart timer */
        memset(G.sharedkey, '\x00', 512*3);
        G.sk_start = time(NULL);
    }

    /* is auth packet */
    if( (h80211[1] & 0x40) != 0x40 )
    {
        /* not encrypted */
        if( ( h80211[24] + (h80211[25] << 8) ) == 1 )
        {
            /* Shared-Key Authentication */
            if( ( h80211[26] + (h80211[27] << 8) ) == 2 )
            {
                /* sequence == 2 */
                memcpy(G.sharedkey[0], h80211, caplen);
                G.sk_len = caplen-24;
            }
            if( ( h80211[26] + (h80211[27] << 8) ) == 4 )
            {
                /* sequence == 4 */
                memcpy(G.sharedkey[2], h80211, caplen);
            }
        }
        else return 1;
    }
    else
    {
        /* encrypted */
        memcpy(G.sharedkey[1], h80211, caplen);
    }

    /* check if the 3 packets form a proper authentication */

    if( ( memcmp(G.sharedkey[0]+m_bmac, NULL_MAC, 6) == 0 ) ||
        ( memcmp(G.sharedkey[1]+m_bmac, NULL_MAC, 6) == 0 ) ||
        ( memcmp(G.sharedkey[2]+m_bmac, NULL_MAC, 6) == 0 ) ) /* some bssids == zero */
    {
        return 1;
    }

    if( ( memcmp(G.sharedkey[0]+m_bmac, G.sharedkey[1]+m_bmac, 6) != 0 ) ||
        ( memcmp(G.sharedkey[0]+m_bmac, G.sharedkey[2]+m_bmac, 6) != 0 ) ) /* all bssids aren't equal */
    {
        return 1;
    }

    if( ( memcmp(G.sharedkey[0]+m_smac, G.sharedkey[2]+m_smac, 6) != 0 ) ||
        ( memcmp(G.sharedkey[0]+m_smac, G.sharedkey[1]+m_dmac, 6) != 0 ) ) /* SA in 2&4 != DA in 3 */
    {
        return 1;
    }

    if( (memcmp(G.sharedkey[0]+m_dmac, G.sharedkey[2]+m_dmac, 6) != 0 ) ||
        (memcmp(G.sharedkey[0]+m_dmac, G.sharedkey[1]+m_smac, 6) != 0 ) ) /* DA in 2&4 != SA in 3 */
    {
        return 1;
    }

    textlen = G.sk_len;

    if((unsigned)textlen > sizeof(text) - 4) return 1;

    memcpy(text, G.sharedkey[0]+24, textlen);

    /* increment sequence number from 2 to 3 */
    text[2] = text[2]+1;

    crc = 0xFFFFFFFF;

    for( n = 0; n < textlen; n++ )
        crc = crc_tbl[(crc ^ text[n]) & 0xFF] ^ (crc >> 8);

    crc = ~crc;

    /* append crc32 over body */
    text[textlen]     = (crc      ) & 0xFF;
    text[textlen+1]   = (crc >>  8) & 0xFF;
    text[textlen+2]   = (crc >> 16) & 0xFF;
    text[textlen+3]   = (crc >> 24) & 0xFF;

    /* cleartext XOR cipher */
    for(n=0; n<(textlen+4); n++)
    {
        prga[4+n] = (text[n] ^ G.sharedkey[1][28+n]) & 0xFF;
    }

    /* write IV+index */
    prga[0] = G.sharedkey[1][24] & 0xFF;
    prga[1] = G.sharedkey[1][25] & 0xFF;
    prga[2] = G.sharedkey[1][26] & 0xFF;
    prga[3] = G.sharedkey[1][27] & 0xFF;

    if( G.f_xor != NULL )
    {
        fclose(G.f_xor);
        G.f_xor = NULL;
    }

    snprintf( ofn, sizeof( ofn ) - 1, "%s-%02d-%02X-%02X-%02X-%02X-%02X-%02X.%s", G.prefix, G.f_index,
              *(G.sharedkey[0]+m_bmac), *(G.sharedkey[0]+m_bmac+1), *(G.sharedkey[0]+m_bmac+2),
              *(G.sharedkey[0]+m_bmac+3), *(G.sharedkey[0]+m_bmac+4), *(G.sharedkey[0]+m_bmac+5), "xor" );

    G.f_xor = fopen( ofn, "w");
    if(G.f_xor == NULL)
        return 1;

    for(n=0; n<textlen+8; n++)
        fputc((prga[n] & 0xFF), G.f_xor);

    fflush(G.f_xor);

    if( G.f_xor != NULL )
    {
        fclose(G.f_xor);
        G.f_xor = NULL;
    }

    snprintf(G.message, sizeof(G.message), "][ %d bytes keystream: %02X:%02X:%02X:%02X:%02X:%02X ",
                textlen+4, *(G.sharedkey[0]+m_bmac), *(G.sharedkey[0]+m_bmac+1), *(G.sharedkey[0]+m_bmac+2),
              *(G.sharedkey[0]+m_bmac+3), *(G.sharedkey[0]+m_bmac+4), *(G.sharedkey[0]+m_bmac+5));

    memset(G.sharedkey, '\x00', 512*3);
    /* ok, keystream saved */
    return 0;
}

char usage[] =

"\n"
"  %s - (C) 2006,2007,2008 Thomas d\'Otreppe\n"
"  Original work: Christophe Devine\n"
"  http://www.aircrack-ng.org\n"
"\n"
"  usage: airodump-ng <options> <interface>[,<interface>,...]\n"
"\n"
"  Options:\n"
"      --ivs               : Save only captured IVs\n"
"      --gpsd              : Use GPSd\n"
"      --write    <prefix> : Dump file prefix\n"
"      -w                  : same as --write \n"
"      --beacons           : Record all beacons in dump file\n"
"      --update     <secs> : Display update delay in seconds\n"
"      --showack           : Prints ack/cts/rts statistics\n"
"      -h                  : Hides known stations for --showack\n"
"      -f          <msecs> : Time in ms between hopping channels\n"
"      --berlin     <secs> : Time before removing the AP/client\n"
"                            from the screen when no more packets\n"
"                            are received (Default: 120 seconds).\n"
"      -r           <file> : Read packets from that file\n"
"\n"
"  Filter options:\n"
"      --encrypt   <suite> : Filter APs by cipher suite\n"
"      --netmask <netmask> : Filter APs by mask\n"
"      --bssid     <bssid> : Filter APs by BSSID\n"
"      -a                  : Filter unassociated clients\n"
"\n"
"  By default, airodump-ng hop on 2.4Ghz channels.\n"
"  You can make it capture on other/specific channel(s) by using:\n"
"      --channel <channels>: Capture on specific channels\n"
"      --band <abg>        : Band on which airodump-ng should hop\n"
"      --cswitch  <method> : Set channel switching method\n"
"                    0     : FIFO (default)\n"
"                    1     : Round Robin\n"
"                    2     : Hop on last\n"
"      -s                  : same as --cswitch\n"
"\n"
"      --help              : Displays this usage screen\n"
"\n";

int is_filtered_netmask(uchar *bssid)
{
    uchar mac1[6];
    uchar mac2[6];
    int i;

    for(i=0; i<6; i++)
    {
        mac1[i] = bssid[i]     & G.f_netmask[i];
        mac2[i] = G.f_bssid[i] & G.f_netmask[i];
    }

    if( memcmp(mac1, mac2, 6) != 0 )
    {
        return( 1 );
    }

    return 0;
}

void update_rx_quality( )
{
    unsigned int time_diff, capt_time, miss_time;
    int missed_frames;
    struct AP_info *ap_cur = NULL;
    struct ST_info *st_cur = NULL;
    struct timeval cur_time;

    ap_cur = G.ap_1st;
    st_cur = G.st_1st;

    gettimeofday( &cur_time, NULL );

    /* accesspoints */
    while( ap_cur != NULL )
    {
        time_diff = 1000000 * (cur_time.tv_sec  - ap_cur->ftimer.tv_sec )
                            + (cur_time.tv_usec - ap_cur->ftimer.tv_usec);

        /* update every `QLT_TIME`seconds if the rate is low, or every 500ms otherwise */
        if( (ap_cur->fcapt >= QLT_COUNT && time_diff > 500000 ) || time_diff > (QLT_TIME * 1000000) )
        {
            /* at least one frame captured */
            if(ap_cur->fcapt > 1)
            {
                capt_time =   ( 1000000 * (ap_cur->ftimel.tv_sec  - ap_cur->ftimef.tv_sec )    //time between first and last captured frame
                                        + (ap_cur->ftimel.tv_usec - ap_cur->ftimef.tv_usec) );

                miss_time =   ( 1000000 * (ap_cur->ftimef.tv_sec  - ap_cur->ftimer.tv_sec )    //time between timer reset and first frame
                                        + (ap_cur->ftimef.tv_usec - ap_cur->ftimer.tv_usec) )
                            + ( 1000000 * (cur_time.tv_sec  - ap_cur->ftimel.tv_sec )          //time between last frame and this moment
                                        + (cur_time.tv_usec - ap_cur->ftimel.tv_usec) );

                //number of frames missed at the time where no frames were captured; extrapolated by assuming a constant framerate
                if(capt_time > 0 && miss_time > 200000)
                {
                    missed_frames = ((float)((float)miss_time/(float)capt_time) * ((float)ap_cur->fcapt + (float)ap_cur->fmiss));
                    ap_cur->fmiss += missed_frames;
                }

                ap_cur->rx_quality = ((float)((float)ap_cur->fcapt / ((float)ap_cur->fcapt + (float)ap_cur->fmiss)) * 100.0);
            }
            else ap_cur->rx_quality = 0; /* no packets -> zero quality */

            /* normalize, in case the seq numbers are not iterating */
            if(ap_cur->rx_quality > 100) ap_cur->rx_quality = 100;
            if(ap_cur->rx_quality < 0  ) ap_cur->rx_quality =   0;

            /* reset variables */
            ap_cur->fcapt = 0;
            ap_cur->fmiss = 0;
            gettimeofday( &(ap_cur->ftimer) ,NULL);
        }
        ap_cur = ap_cur->next;
    }

    /* stations */
    while( st_cur != NULL )
    {
        time_diff = 1000000 * (cur_time.tv_sec  - st_cur->ftimer.tv_sec )
                            + (cur_time.tv_usec - st_cur->ftimer.tv_usec);

        if( time_diff > 10000000 )
        {
            st_cur->missed = 0;
            gettimeofday( &(st_cur->ftimer), NULL );
        }

        st_cur = st_cur->next;
    }

}

/* setup the output files */

int dump_initialize( char *prefix, int ivs_only )
{
    int i;
    FILE *f;
    char ofn[1024];


    /* If you only want to see what happening, send all data to /dev/null */

    if ( prefix == NULL) {
	    return( 0 );
    }

    /* check not to overflow the ofn buffer */

    if( strlen( prefix ) >= sizeof( ofn ) - 10 )
        prefix[sizeof( ofn ) - 10] = '\0';

    /* make sure not to overwrite any existing file */

    memset( ofn, 0, sizeof( ofn ) );

    G.f_index = 1;

    do
    {
        for( i = 0; i < 4; i++ )
        {
            snprintf( ofn,  sizeof( ofn ) - 1, "%s-%02d.%s",
                      prefix, G.f_index, f_ext[i] );

            if( ( f = fopen( ofn, "rb+" ) ) != NULL )
            {
                fclose( f );
                G.f_index++;
                break;
            }
        }
    }
    while( i < 4 );

    G.prefix = (char*) malloc(strlen(prefix)+2);
    snprintf(G.prefix, strlen(prefix)+1, "%s", prefix);

    /* create the output CSV & GPS files */

    snprintf( ofn,  sizeof( ofn ) - 1, "%s-%02d.txt",
              prefix, G.f_index );

    if( ( G.f_txt = fopen( ofn, "wb+" ) ) == NULL )
    {
        perror( "fopen failed" );
        fprintf( stderr, "Could not create \"%s\".\n", ofn );
        return( 1 );
    }

    if (G.usegpsd)
    {
        snprintf( ofn,  sizeof( ofn ) - 1, "%s-%02d.gps",
                  prefix, G.f_index );

        if( ( G.f_gps = fopen( ofn, "wb+" ) ) == NULL )
        {
            perror( "fopen failed" );
            fprintf( stderr, "Could not create \"%s\".\n", ofn );
            return( 1 );
        }
    }
    /* create the output packet capture file */

    if( ivs_only == 0 )
    {
        struct pcap_file_header pfh;

        snprintf( ofn,  sizeof( ofn ) - 1, "%s-%02d.cap",
                  prefix, G.f_index );

        if( ( G.f_cap = fopen( ofn, "wb+" ) ) == NULL )
        {
            perror( "fopen failed" );
            fprintf( stderr, "Could not create \"%s\".\n", ofn );
            return( 1 );
        }

        G.f_cap_name = (char*) malloc(128);
        snprintf(G.f_cap_name, 127, "%s",ofn);

        pfh.magic           = TCPDUMP_MAGIC;
        pfh.version_major   = PCAP_VERSION_MAJOR;
        pfh.version_minor   = PCAP_VERSION_MINOR;
        pfh.thiszone        = 0;
        pfh.sigfigs         = 0;
        pfh.snaplen         = 65535;
        pfh.linktype        = LINKTYPE_IEEE802_11;

        if( fwrite( &pfh, 1, sizeof( pfh ), G.f_cap ) !=
                    (size_t) sizeof( pfh ) )
        {
            perror( "fwrite(pcap file header) failed" );
            return( 1 );
        }
    } else {
        struct ivs2_filehdr fivs2;

        fivs2.version = IVS2_VERSION;

        snprintf( ofn,  sizeof( ofn ) - 1, "%s-%02d.%s",
                  prefix, G.f_index, IVS2_EXTENSION );

        if( ( G.f_ivs = fopen( ofn, "wb+" ) ) == NULL )
        {
            perror( "fopen failed" );
            fprintf( stderr, "Could not create \"%s\".\n", ofn );
            return( 1 );
        }

        if( fwrite( IVS2_MAGIC, 1, 4, G.f_ivs ) != (size_t) 4 )
        {
            perror( "fwrite(IVs file MAGIC) failed" );
            return( 1 );
        }

        if( fwrite( &fivs2, 1, sizeof(struct ivs2_filehdr), G.f_ivs ) != (size_t) sizeof(struct ivs2_filehdr) )
        {
            perror( "fwrite(IVs file header) failed" );
            return( 1 );
        }
    }

    return( 0 );
}

int update_dataps()
{
    struct timeval tv;
    struct AP_info *ap_cur;
    struct NA_info *na_cur;
    int sec, usec, diff, ps;
    float pause;

    gettimeofday(&tv, NULL);

    ap_cur = G.ap_end;

    while( ap_cur != NULL )
    {
        sec = (tv.tv_sec - ap_cur->tv.tv_sec);
        usec = (tv.tv_usec - ap_cur->tv.tv_usec);
        pause = (((float)(sec*1000000.0f + usec))/(1000000.0f));
        if( pause > 2.0f )
        {
            diff = ap_cur->nb_data - ap_cur->nb_data_old;
            ps = (int)(((float)diff)/pause);
            ap_cur->nb_dataps = ps;
            ap_cur->nb_data_old = ap_cur->nb_data;
            gettimeofday(&(ap_cur->tv), NULL);
        }
        ap_cur = ap_cur->prev;
    }

    na_cur = G.na_1st;

    while( na_cur != NULL )
    {
        sec = (tv.tv_sec - na_cur->tv.tv_sec);
        usec = (tv.tv_usec - na_cur->tv.tv_usec);
        pause = (((float)(sec*1000000.0f + usec))/(1000000.0f));
        if( pause > 2.0f )
        {
            diff = na_cur->ack - na_cur->ack_old;
            ps = (int)(((float)diff)/pause);
            na_cur->ackps = ps;
            na_cur->ack_old = na_cur->ack;
            gettimeofday(&(na_cur->tv), NULL);
        }
        na_cur = na_cur->next;
    }
    return(0);
}

int list_tail_free(struct pkt_buf **list)
{
    struct pkt_buf **pkts;
    struct pkt_buf *next;

    if(list == NULL) return 1;

    pkts = list;

    while(*pkts != NULL)
    {
        next = (*pkts)->next;
        if( (*pkts)->packet )
        {
            free( (*pkts)->packet);
            (*pkts)->packet=NULL;
        }

        if(*pkts)
        {
            free(*pkts);
            *pkts = NULL;
        }
        *pkts = next;
    }

    *list=NULL;

    return 0;
}

int list_add_packet(struct pkt_buf **list, int length, unsigned char* packet)
{
    struct pkt_buf *next = *list;

    if(length <= 0) return 1;
    if(packet == NULL) return 1;
    if(list == NULL) return 1;

    *list = (struct pkt_buf*) malloc(sizeof(struct pkt_buf));
    if( *list == NULL ) return 1;
    (*list)->packet = (unsigned char*) malloc(length);
    if( (*list)->packet == NULL ) return 1;

    memcpy((*list)->packet,  packet, length);
    (*list)->next = next;
    (*list)->length = length;
    gettimeofday( &((*list)->ctime), NULL);

    return 0;
}

int list_check_decloak(struct pkt_buf **list, int length, unsigned char* packet)
{
    struct pkt_buf *next = *list;
    struct timeval tv1;
    int timediff;
    int i, correct;

    if( packet == NULL) return 1;
    if( list == NULL ) return 1;
    if( *list == NULL ) return 1;
    if( length <= 0) return 1;

    gettimeofday(&tv1, NULL);

    timediff = (((tv1.tv_sec - ((*list)->ctime.tv_sec)) * 1000000) + (tv1.tv_usec - ((*list)->ctime.tv_usec))) / 1000;
    if( timediff > BUFFER_TIME )
    {
        list_tail_free(list);
        next=NULL;
    }

    while(next != NULL)
    {
        if(next->next != NULL)
        {
            timediff = (((tv1.tv_sec - (next->next->ctime.tv_sec)) * 1000000) + (tv1.tv_usec - (next->next->ctime.tv_usec))) / 1000;
            if( timediff > BUFFER_TIME )
            {
                list_tail_free(&(next->next));
                break;
            }
        }
        if( (next->length + 4) == length)
        {
            correct = 1;
            // check for 4 bytes added after the end
            for(i=28;i<length-28;i++)   //check everything (in the old packet) after the IV (including crc32 at the end)
            {
                if(next->packet[i] != packet[i])
                {
                    correct = 0;
                    break;
                }
            }
            if(!correct)
            {
                correct = 1;
                // check for 4 bytes added at the beginning
                for(i=28;i<length-28;i++)   //check everything (in the old packet) after the IV (including crc32 at the end)
                {
                    if(next->packet[i] != packet[4+i])
                    {
                        correct = 0;
                        break;
                    }
                }
            }
            if(correct == 1)
                    return 0;   //found decloaking!
        }
        next = next->next;
    }

    return 1; //didn't find decloak
}

int remove_namac(unsigned char* mac)
{
    struct NA_info *na_cur = NULL;
    struct NA_info *na_prv = NULL;

    if(mac == NULL)
        return( -1 );

    na_cur = G.na_1st;
    na_prv = NULL;

    while( na_cur != NULL )
    {
        if( ! memcmp( na_cur->namac, mac, 6 ) )
            break;

        na_prv = na_cur;
        na_cur = na_cur->next;
    }

    /* if it's known, remove it */
    if( na_cur != NULL )
    {
        /* first in linked list */
        if(na_cur == G.na_1st)
        {
            G.na_1st = na_cur->next;
        }
        else
        {
            na_prv->next = na_cur->next;
        }
        free(na_cur);
        na_cur=NULL;
    }

    return( 0 );
}

int dump_add_packet( unsigned char *h80211, int caplen, struct rx_info *ri, int cardnum )
{
    int i, n, z, seq, msd, dlen, offset, clen, o;
    int type, length, numuni=0, numauth=0;
    struct pcap_pkthdr pkh;
    struct timeval tv;
    struct ivs2_pkthdr ivs2;
    unsigned char *p, c;
    unsigned char bssid[6];
    unsigned char stmac[6];
    unsigned char namac[6];
    unsigned char clear[2048];
    int weight[16];
    int num_xor=0;

    struct AP_info *ap_cur = NULL;
    struct ST_info *st_cur = NULL;
    struct NA_info *na_cur = NULL;
    struct AP_info *ap_prv = NULL;
    struct ST_info *st_prv = NULL;
    struct NA_info *na_prv = NULL;

    /* skip packets smaller than a 802.11 header */

    if( caplen < 24 )
        goto write_packet;

    /* skip (uninteresting) control frames */

    if( ( h80211[0] & 0x0C ) == 0x04 )
        goto write_packet;

    /* if it's a LLC null packet, just forget it (may change in the future) */

    if ( caplen > 28)
        if ( memcmp(h80211 + 24, llcnull, 4) == 0)
            return ( 0 );

    /* grab the sequence number */
    seq = ((h80211[22]>>4)+(h80211[23]<<4));

    /* locate the access point's MAC address */

    switch( h80211[1] & 3 )
    {
        case  0: memcpy( bssid, h80211 + 16, 6 ); break;  //Adhoc
        case  1: memcpy( bssid, h80211 +  4, 6 ); break;  //ToDS
        case  2: memcpy( bssid, h80211 + 10, 6 ); break;  //FromDS
        case  3: memcpy( bssid, h80211 + 10, 6 ); break;  //WDS -> Transmitter taken as BSSID
    }

    if( memcmp(G.f_bssid, NULL_MAC, 6) != 0 )
    {
        if( memcmp(G.f_netmask, NULL_MAC, 6) != 0 )
        {
            if(is_filtered_netmask(bssid)) return(1);
        }
        else
        {
            if( memcmp(G.f_bssid, bssid, 6) != 0 ) return(1);
        }
    }

    /* update our chained list of access points */

    ap_cur = G.ap_1st;
    ap_prv = NULL;

    while( ap_cur != NULL )
    {
        if( ! memcmp( ap_cur->bssid, bssid, 6 ) )
            break;

        ap_prv = ap_cur;
        ap_cur = ap_cur->next;
    }

    /* if it's a new access point, add it */

    if( ap_cur == NULL )
    {
        if( ! ( ap_cur = (struct AP_info *) malloc(
                         sizeof( struct AP_info ) ) ) )
        {
            perror( "malloc failed" );
            return( 1 );
        }

        /* if mac is listed as unknown, remove it */
        remove_namac(bssid);

        memset( ap_cur, 0, sizeof( struct AP_info ) );

        if( G.ap_1st == NULL )
            G.ap_1st = ap_cur;
        else
            ap_prv->next  = ap_cur;

        memcpy( ap_cur->bssid, bssid, 6 );

        ap_cur->prev = ap_prv;

        ap_cur->tinit = time( NULL );
        ap_cur->tlast = time( NULL );

        ap_cur->avg_power   = -1;
        ap_cur->power_index = -1;

        for( i = 0; i < NB_PWR; i++ )
            ap_cur->power_lvl[i] = -1;

        ap_cur->channel    = -1;
        ap_cur->max_speed  = -1;
        ap_cur->security   = 0;

        ap_cur->uiv_root = uniqueiv_init();

        ap_cur->nb_dataps = 0;
        ap_cur->nb_data_old = 0;
        gettimeofday(&(ap_cur->tv), NULL);

        ap_cur->dict_started = 0;

        ap_cur->key = NULL;

        G.ap_end = ap_cur;

        ap_cur->nb_bcn     = 0;

        ap_cur->rx_quality = 0;
        ap_cur->fcapt      = 0;
        ap_cur->fmiss      = 0;
        ap_cur->last_seq   = 0;
        gettimeofday( &(ap_cur->ftimef), NULL);
        gettimeofday( &(ap_cur->ftimel), NULL);
        gettimeofday( &(ap_cur->ftimer), NULL);

        ap_cur->ssid_length = 0;
        ap_cur->essid_stored = 0;

        ap_cur->decloak_detect=G.decloak;
        ap_cur->is_decloak = 0;
        ap_cur->packets = NULL;
        ap_cur->data_root = NULL;
        ap_cur->EAP_detected = 0;
    }

    /* update the last time seen */

    ap_cur->tlast = time( NULL );

    /* only update power if packets comes from
     * the AP: either type == mgmt and SA != BSSID,
     * or FromDS == 1 and ToDS == 0 */

    if( ( ( h80211[1] & 3 ) == 0 &&
            memcmp( h80211 + 10, bssid, 6 ) == 0 ) ||
        ( ( h80211[1] & 3 ) == 2 ) )
    {
        ap_cur->power_index = ( ap_cur->power_index + 1 ) % NB_PWR;
        ap_cur->power_lvl[ap_cur->power_index] = ri->ri_power;

        ap_cur->avg_power = 0;

        for( i = 0, n = 0; i < NB_PWR; i++ )
        {
            if( ap_cur->power_lvl[i] != -1 )
            {
                ap_cur->avg_power += ap_cur->power_lvl[i];
                n++;
            }
        }

        if( n > 0 )
            ap_cur->avg_power /= n;
        else
            ap_cur->avg_power = -1;

        /* every packet in here comes from the AP */

//        printf("seqnum: %i\n", seq);

        if(ap_cur->fcapt == 0 && ap_cur->fmiss == 0) gettimeofday( &(ap_cur->ftimef), NULL);
        if(ap_cur->last_seq != 0) ap_cur->fmiss += (seq - ap_cur->last_seq - 1);
        ap_cur->last_seq = seq;
        ap_cur->fcapt++;
        gettimeofday( &(ap_cur->ftimel), NULL);

//         if(ap_cur->fcapt >= QLT_COUNT) update_rx_quality();
    }

    if( h80211[0] == 0x80 )
    {
        ap_cur->nb_bcn++;
    }

    ap_cur->nb_pkt++;

    /* find wpa handshake */
    if( h80211[0] == 0x10 )
    {
        /* reset the WPA handshake state */

        if( st_cur != NULL && st_cur->wpa.state != 0xFF )
            st_cur->wpa.state = 0;
//        printf("initial auth %d\n", ap_cur->wpa_state);
    }

    /* locate the station MAC in the 802.11 header */

    switch( h80211[1] & 3 )
    {
        case  0:

            /* if management, check that SA != BSSID */

            if( memcmp( h80211 + 10, bssid, 6 ) == 0 )
                goto skip_station;

            memcpy( stmac, h80211 + 10, 6 );
            break;

        case  1:

            /* ToDS packet, must come from a client */

            memcpy( stmac, h80211 + 10, 6 );
            break;

        case  2:

            /* FromDS packet, reject broadcast MACs */

            if( h80211[4] != 0 ) goto skip_station;
            memcpy( stmac, h80211 +  4, 6 ); break;

        default: goto skip_station;
    }

    /* update our chained list of wireless stations */

    st_cur = G.st_1st;
    st_prv = NULL;

    while( st_cur != NULL )
    {
        if( ! memcmp( st_cur->stmac, stmac, 6 ) )
            break;

        st_prv = st_cur;
        st_cur = st_cur->next;
    }

    /* if it's a new client, add it */

    if( st_cur == NULL )
    {
        if( ! ( st_cur = (struct ST_info *) malloc(
                         sizeof( struct ST_info ) ) ) )
        {
            perror( "malloc failed" );
            return( 1 );
        }

        /* if mac is listed as unknown, remove it */
        remove_namac(stmac);

        memset( st_cur, 0, sizeof( struct ST_info ) );

        if( G.st_1st == NULL )
            G.st_1st = st_cur;
        else
            st_prv->next  = st_cur;

        memcpy( st_cur->stmac, stmac, 6 );

        st_cur->prev = st_prv;

        st_cur->tinit = time( NULL );
        st_cur->tlast = time( NULL );

        st_cur->power = -1;
        st_cur->rate_to = -1;
        st_cur->rate_from = -1;

        st_cur->probe_index = -1;
        st_cur->missed  = 0;
        st_cur->lastseq = 0;
        gettimeofday( &(st_cur->ftimer), NULL);

        for( i = 0; i < NB_PRB; i++ )
        {
            memset( st_cur->probes[i], 0, sizeof(
                    st_cur->probes[i] ) );
            st_cur->ssid_length[i] = 0;
        }

        G.st_end = st_cur;
    }

    if( st_cur->base == NULL ||
        memcmp( ap_cur->bssid, BROADCAST, 6 ) != 0 )
        st_cur->base = ap_cur;

    //update bitrate to station
    if( (st_cur != NULL) && ( h80211[1] & 3 ) == 2 )
        st_cur->rate_to = ri->ri_rate;

    /* update the last time seen */

    st_cur->tlast = time( NULL );

    /* only update power if packets comes from the
     * client: either type == Mgmt and SA != BSSID,
     * or FromDS == 0 and ToDS == 1 */

    if( ( ( h80211[1] & 3 ) == 0 &&
            memcmp( h80211 + 10, bssid, 6 ) != 0 ) ||
        ( ( h80211[1] & 3 ) == 1 ) )
    {
        st_cur->power = ri->ri_power;
        st_cur->rate_from = ri->ri_rate;

        if(st_cur->lastseq != 0)
        {
            msd = seq - st_cur->lastseq - 1;
            if(msd > 0 && msd < 1000)
                st_cur->missed += msd;
        }
        st_cur->lastseq = seq;
    }

    st_cur->nb_pkt++;

skip_station:

    /* packet parsing: Probe Request */

    if( h80211[0] == 0x40 && st_cur != NULL )
    {
        p = h80211 + 24;

        while( p < h80211 + caplen )
        {
            if( p + 2 + p[1] > h80211 + caplen )
                break;

            if( p[0] == 0x00 && p[1] > 0 && p[2] != '\0' &&
                ( p[1] > 1 || p[2] != ' ' ) )
            {
//                n = ( p[1] > 32 ) ? 32 : p[1];
                n = p[1];

                for( i = 0; i < n; i++ )
                    if( p[2 + i] > 0 && p[2 + i] < ' ' )
                        goto skip_probe;

                /* got a valid ASCII probed ESSID, check if it's
                   already in the ring buffer */

                for( i = 0; i < NB_PRB; i++ )
                    if( memcmp( st_cur->probes[i], p + 2, n ) == 0 )
                        goto skip_probe;

                st_cur->probe_index = ( st_cur->probe_index + 1 ) % NB_PRB;
                memset( st_cur->probes[st_cur->probe_index], 0, 256 );
                memcpy( st_cur->probes[st_cur->probe_index], p + 2, n ); //twice?!
                st_cur->ssid_length[st_cur->probe_index] = n;

                for( i = 0; i < n; i++ )
                {
                    c = p[2 + i];
                    if( c == 0 || ( c > 126 && c < 160 ) ) c = '.';  //could also check ||(c>0 && c<32)
                    st_cur->probes[st_cur->probe_index][i] = c;
                }
            }

            p += 2 + p[1];
        }
    }

skip_probe:

    /* packet parsing: Beacon or Probe Response */

    if( h80211[0] == 0x80 || h80211[0] == 0x50 )
    {
        if( !(ap_cur->security & (STD_OPN|STD_WEP|STD_WPA|STD_WPA2)) )
        {
            if( ( h80211[34] & 0x10 ) >> 4 ) ap_cur->security |= STD_WEP|ENC_WEP;
            else ap_cur->security |= STD_OPN;
        }

        ap_cur->preamble = ( h80211[34] & 0x20 ) >> 5;

        p = h80211 + 36;

        while( p < h80211 + caplen )
        {
            if( p + 2 + p[1] > h80211 + caplen )
                break;

            //only update the essid length if the new length is > the old one
            if( p[0] == 0x00 && (ap_cur->ssid_length < p[1]) ) ap_cur->ssid_length = p[1];

            if( p[0] == 0x00 && p[1] > 0 && p[2] != '\0' &&
                ( p[1] > 1 || p[2] != ' ' ) )
            {
                /* found a non-cloaked ESSID */

//                n = ( p[1] > 32 ) ? 32 : p[1];
                n = p[1];

                memset( ap_cur->essid, 0, 256 );
                memcpy( ap_cur->essid, p + 2, n );

                if( G.f_ivs != NULL && !ap_cur->essid_stored )
                {
                    memset(&ivs2, '\x00', sizeof(struct ivs2_pkthdr));
                    ivs2.flags |= IVS2_ESSID;
                    ivs2.len += ap_cur->ssid_length;

                    if( memcmp( G.prev_bssid, ap_cur->bssid, 6 ) != 0 )
                    {
                        ivs2.flags |= IVS2_BSSID;
                        ivs2.len += 6;
                        memcpy( G.prev_bssid, ap_cur->bssid,  6 );
                    }

                    /* write header */
                    if( fwrite( &ivs2, 1, sizeof(struct ivs2_pkthdr), G.f_ivs )
                        != (size_t) sizeof(struct ivs2_pkthdr) )
                    {
                        perror( "fwrite(IV header) failed" );
                        return( 1 );
                    }

                    /* write BSSID */
                    if(ivs2.flags & IVS2_BSSID)
                    {
                        if( fwrite( ap_cur->bssid, 1, 6, G.f_ivs )
                            != (size_t) 6 )
                        {
                            perror( "fwrite(IV bssid) failed" );
                            return( 1 );
                        }
                    }

                    /* write essid */
                    if( fwrite( ap_cur->essid, 1, ap_cur->ssid_length, G.f_ivs )
                        != (size_t) ap_cur->ssid_length )
                    {
                        perror( "fwrite(IV essid) failed" );
                        return( 1 );
                    }

                    ap_cur->essid_stored = 1;
                }

                for( i = 0; i < n; i++ )
                    if( ( ap_cur->essid[i] >   0 && ap_cur->essid[i] <  32 ) ||
                        ( ap_cur->essid[i] > 126 && ap_cur->essid[i] < 160 ) )
                        ap_cur->essid[i] = '.';
            }

            /* get the maximum speed in Mb and the AP's channel */

            if( p[0] == 0x01 || p[0] == 0x32 )
                ap_cur->max_speed = ( p[1 + p[1]] & 0x7F ) / 2;

            if( p[0] == 0x03 )
                ap_cur->channel = p[2];

            p += 2 + p[1];
        }
    }

    /* packet parsing: Beacon */

    if( h80211[0] == 0x80 && caplen > 38)
    {
        p=h80211+36;         //ignore hdr + fixed params

        while( p < h80211 + caplen )
        {
            type = p[0];
            length = p[1];
            if(p+2+length > h80211 + caplen)
                break;

            if( (type == 0xDD && (length >= 8) && (memcmp(p+2, "\x00\x50\xF2\x01\x01\x00", 6) == 0)) || (type == 0x30) )
            {
                ap_cur->security &= ~(STD_WEP|ENC_WEP|STD_WPA);

                offset = 0;

                if(type == 0xDD)
                {
                    //WPA defined in vendor specific tag -> WPA1 support
                    ap_cur->security |= STD_WPA;
                    offset = 4;
                }

                if(type == 0x30)
                {
                    ap_cur->security |= STD_WPA2;
                    offset = 0;
                }

                if(length < (18+offset))
                {
                    p += length+2;
                    continue;
                }

                if( p+9+offset > h80211+caplen )
                    break;
                numuni  = p[8+offset] + (p[9+offset]<<8);

                if( p+ (11+offset) + 4*numuni > h80211+caplen)
                    break;
                numauth = p[(10+offset) + 4*numuni] + (p[(11+offset) + 4*numuni]<<8);

                p += (10+offset);

                if(type != 0x30)
                {
                    if( p + (4*numuni) + (2+4*numauth) > h80211+caplen)
                        break;
                }
                else
                {
                    if( p + (4*numuni) + (2+4*numauth) + 2 > h80211+caplen)
                        break;
                }

                for(i=0; i<numuni; i++)
                {
                    switch(p[i*4+3])
                    {
                    case 0x01:
                        ap_cur->security |= ENC_WEP;
                        break;
                    case 0x02:
                        ap_cur->security |= ENC_TKIP;
                        break;
                    case 0x03:
                        ap_cur->security |= ENC_WRAP;
                        break;
                    case 0x04:
                        ap_cur->security |= ENC_CCMP;
                        break;
                    case 0x05:
                        ap_cur->security |= ENC_WEP104;
                        break;
                    default:
                        break;
                    }
                }

                p += 2+4*numuni;

                for(i=0; i<numauth; i++)
                {
                    switch(p[i*4+3])
                    {
                    case 0x01:
                        ap_cur->security |= AUTH_MGT;
                        break;
                    case 0x02:
                        ap_cur->security |= AUTH_PSK;
                        break;
                    default:
                        break;
                    }
                }

                p += 2+4*numauth;

                if( type == 0x30 ) p += 2;

            }
            else p += length+2;
        }
    }

    /* packet parsing: Authentication Response */

    if( h80211[0] == 0xB0 && caplen >= 30)
    {
        if( ap_cur->security & STD_WEP )
        {
            //successful step 2 or 4 (coming from the AP)
            if(memcmp(h80211+28, "\x00\x00", 2) == 0 &&
                (h80211[26] == 0x02 || h80211[26] == 0x04))
            {
                ap_cur->security &= ~(AUTH_OPN | AUTH_PSK | AUTH_MGT);
                if(h80211[24] == 0x00) ap_cur->security |= AUTH_OPN;
                if(h80211[24] == 0x01) ap_cur->security |= AUTH_PSK;
            }
        }
    }

    /* packet parsing: Association Request */

    if( h80211[0] == 0x00 && caplen > 28 )
    {
        p = h80211 + 28;

        while( p < h80211 + caplen )
        {
            if( p + 2 + p[1] > h80211 + caplen )
                break;

            if( p[0] == 0x00 && p[1] > 0 && p[2] != '\0' &&
                ( p[1] > 1 || p[2] != ' ' ) )
            {
                /* found a non-cloaked ESSID */

                n = ( p[1] > 32 ) ? 32 : p[1];

                memset( ap_cur->essid, 0, 33 );
                memcpy( ap_cur->essid, p + 2, n );

                if( G.f_ivs != NULL && !ap_cur->essid_stored )
                {
                    memset(&ivs2, '\x00', sizeof(struct ivs2_pkthdr));
                    ivs2.flags |= IVS2_ESSID;
                    ivs2.len += ap_cur->ssid_length;

                    if( memcmp( G.prev_bssid, ap_cur->bssid, 6 ) != 0 )
                    {
                        ivs2.flags |= IVS2_BSSID;
                        ivs2.len += 6;
                        memcpy( G.prev_bssid, ap_cur->bssid,  6 );
                    }

                    /* write header */
                    if( fwrite( &ivs2, 1, sizeof(struct ivs2_pkthdr), G.f_ivs )
                        != (size_t) sizeof(struct ivs2_pkthdr) )
                    {
                        perror( "fwrite(IV header) failed" );
                        return( 1 );
                    }

                    /* write BSSID */
                    if(ivs2.flags & IVS2_BSSID)
                    {
                        if( fwrite( ap_cur->bssid, 1, 6, G.f_ivs )
                            != (size_t) 6 )
                        {
                            perror( "fwrite(IV bssid) failed" );
                            return( 1 );
                        }
                    }

                    /* write essid */
                    if( fwrite( ap_cur->essid, 1, ap_cur->ssid_length, G.f_ivs )
                        != (size_t) ap_cur->ssid_length )
                    {
                        perror( "fwrite(IV essid) failed" );
                        return( 1 );
                    }

                    ap_cur->essid_stored = 1;
                }

                for( i = 0; i < n; i++ )
                    if( ap_cur->essid[i] < 32 ||
                      ( ap_cur->essid[i] > 126 && ap_cur->essid[i] < 160 ) )
                        ap_cur->essid[i] = '.';
            }

            p += 2 + p[1];
        }
        if(st_cur != NULL)
            st_cur->wpa.state = 0;
    }

    /* packet parsing: some data */

    if( ( h80211[0] & 0x0C ) == 0x08 )
    {
        /* update the channel if we didn't get any beacon */

        if( ap_cur->channel == -1 )
        {
            if(ri->ri_channel > 0 && ri->ri_channel < 167)
                ap_cur->channel = ri->ri_channel;
            else
                ap_cur->channel = G.channel[cardnum];
        }

        /* check the SNAP header to see if data is encrypted */

        z = ( ( h80211[1] & 3 ) != 3 ) ? 24 : 30;

        /* Check if 802.11e (QoS) */
        if( (h80211[0] & 0x80) == 0x80) z+=2;

        if(z==24)
        {
            if(list_check_decloak(&(ap_cur->packets), caplen, h80211) != 0)
            {
                list_add_packet(&(ap_cur->packets), caplen, h80211);
            }
            else
            {
                ap_cur->is_decloak = 1;
                ap_cur->decloak_detect = 0;
                list_tail_free(&(ap_cur->packets));
                memset(G.message, '\x00', sizeof(G.message));
                    snprintf( G.message, sizeof( G.message ) - 1,
                        "][ Decloak: %02X:%02X:%02X:%02X:%02X:%02X ",
                        ap_cur->bssid[0], ap_cur->bssid[1], ap_cur->bssid[2],
                        ap_cur->bssid[3], ap_cur->bssid[4], ap_cur->bssid[5]);
            }
        }

        if( z + 26 > caplen )
            goto write_packet;

        if( h80211[z] == h80211[z + 1] && h80211[z + 2] == 0x03 )
        {
//            if( ap_cur->encryption < 0 )
//                ap_cur->encryption = 0;

            /* if ethertype == IPv4, find the LAN address */

            if( h80211[z + 6] == 0x08 && h80211[z + 7] == 0x00 &&
                ( h80211[1] & 3 ) == 0x01 )
                    memcpy( ap_cur->lanip, &h80211[z + 20], 4 );

            if( h80211[z + 6] == 0x08 && h80211[z + 7] == 0x06 )
                memcpy( ap_cur->lanip, &h80211[z + 22], 4 );
        }
//        else
//            ap_cur->encryption = 2 + ( ( h80211[z + 3] & 0x20 ) >> 5 );


        if(ap_cur->security == 0 || (ap_cur->security & STD_WEP) )
        {
            if( (h80211[1] & 0x40) != 0x40 )
            {
                ap_cur->security |= STD_OPN;
            }
            else
            {
                if((h80211[z+3] & 0x20) == 0x20)
                {
                    ap_cur->security |= STD_WPA;
                }
                else
                {
                    ap_cur->security |= STD_WEP;
                    if( (h80211[z+3] & 0xC0) != 0x00)
                    {
                        ap_cur->security |= ENC_WEP40;
                    }
                    else
                    {
                        ap_cur->security &= ~ENC_WEP40;
                        ap_cur->security |= ENC_WEP;
                    }
                }
            }
        }

        if( z + 10 > caplen )
            goto write_packet;

        if( ap_cur->security & STD_WEP )
        {
            /* WEP: check if we've already seen this IV */

            if( ! uniqueiv_check( ap_cur->uiv_root, &h80211[z] ) )
            {
                /* first time seen IVs */

                if( G.f_ivs != NULL )
                {
                    memset(&ivs2, '\x00', sizeof(struct ivs2_pkthdr));
                    ivs2.flags = 0;
                    ivs2.len = 0;

                    /* datalen = caplen - (header+iv+ivs) */
                    dlen = caplen -z -4 -4; //original data len
                    if(dlen > 2048) dlen = 2048;
                    //get cleartext + len + 4(iv+idx)
                    num_xor = known_clear(clear, &clen, weight, h80211, dlen);
                    if(num_xor == 1)
                    {
                        ivs2.flags |= IVS2_XOR;
                        ivs2.len += clen + 4;
                        /* reveal keystream (plain^encrypted) */
                        for(n=0; n<(ivs2.len-4); n++)
                        {
                            clear[n] = (clear[n] ^ h80211[z+4+n]) & 0xFF;
                        }
                        //clear is now the keystream
                    }
                    else
                    {
                        //do it again to get it 2 bytes higher
                        num_xor = known_clear(clear+2, &clen, weight, h80211, dlen);
                        ivs2.flags |= IVS2_PTW;
                        //len = 4(iv+idx) + 1(num of keystreams) + 1(len per keystream) + 32*num_xor + 16*sizeof(int)(weight[16])
                        ivs2.len += 4 + 1 + 1 + 32*num_xor + 16*sizeof(int);
                        clear[0] = num_xor;
                        clear[1] = clen;
                        /* reveal keystream (plain^encrypted) */
                        for(o=0; o<num_xor; o++)
                        {
                            for(n=0; n<(ivs2.len-4); n++)
                            {
                                clear[2+n+o*32] = (clear[2+n+o*32] ^ h80211[z+4+n]) & 0xFF;
                            }
                        }
                        memcpy(clear+4 + 1 + 1 + 32*num_xor, weight, 16*sizeof(int));
                        //clear is now the keystream
                    }

                    if( memcmp( G.prev_bssid, ap_cur->bssid, 6 ) != 0 )
                    {
                        ivs2.flags |= IVS2_BSSID;
                        ivs2.len += 6;
                        memcpy( G.prev_bssid, ap_cur->bssid,  6 );
                    }

                    if( fwrite( &ivs2, 1, sizeof(struct ivs2_pkthdr), G.f_ivs )
                        != (size_t) sizeof(struct ivs2_pkthdr) )
                    {
                        perror( "fwrite(IV header) failed" );
                        return( 1 );
                    }

                    if( ivs2.flags & IVS2_BSSID )
                    {
                        if( fwrite( ap_cur->bssid, 1, 6, G.f_ivs ) != (size_t) 6 )
                        {
                            perror( "fwrite(IV bssid) failed" );
                            return( 1 );
                        }
                        ivs2.len -= 6;
                    }

                    if( fwrite( h80211+z, 1, 4, G.f_ivs ) != (size_t) 4 )
                    {
                        perror( "fwrite(IV iv+idx) failed" );
                        return( 1 );
                    }
                    ivs2.len -= 4;

                    if( fwrite( clear, 1, ivs2.len, G.f_ivs ) != (size_t) ivs2.len )
                    {
                        perror( "fwrite(IV keystream) failed" );
                        return( 1 );
                    }
                }

                uniqueiv_mark( ap_cur->uiv_root, &h80211[z] );

                ap_cur->nb_data++;
            }

            // Record all data linked to IV to detect WEP Cloaking
            if( G.f_ivs == NULL && G.detect_anomaly)
            {
				// Only allocate this when seeing WEP AP
				if (ap_cur->data_root == NULL)
					ap_cur->data_root = data_init();

				// Only works with full capture, not IV-only captures
				if (data_check(ap_cur->data_root, &h80211[z], &h80211[z + 4])
					== CLOAKING && ap_cur->EAP_detected == 0)
				{

					//If no EAP/EAP was detected, indicate WEP cloaking
                    memset(G.message, '\x00', sizeof(G.message));
                    snprintf( G.message, sizeof( G.message ) - 1,
                        "][ WEP Cloaking: %02X:%02X:%02X:%02X:%02X:%02X ",
                        ap_cur->bssid[0], ap_cur->bssid[1], ap_cur->bssid[2],
                        ap_cur->bssid[3], ap_cur->bssid[4], ap_cur->bssid[5]);

				}
			}

        }
        else
        {
            ap_cur->nb_data++;
        }

        z = ( ( h80211[1] & 3 ) != 3 ) ? 24 : 30;

        /* Check if 802.11e (QoS) */
        if( (h80211[0] & 0x80) == 0x80) z+=2;

        if( z + 26 > caplen )
            goto write_packet;

        z += 6;     //skip LLC header

        /* check ethertype == EAPOL */
        if( h80211[z] == 0x88 && h80211[z + 1] == 0x8E && (h80211[1] & 0x40) != 0x40 )
        {
			ap_cur->EAP_detected = 1;

            z += 2;     //skip ethertype

            if( st_cur == NULL )
                goto write_packet;

            /* frame 1: Pairwise == 1, Install == 0, Ack == 1, MIC == 0 */

            if( ( h80211[z + 6] & 0x08 ) != 0 &&
                  ( h80211[z + 6] & 0x40 ) == 0 &&
                  ( h80211[z + 6] & 0x80 ) != 0 &&
                  ( h80211[z + 5] & 0x01 ) == 0 )
            {
                memcpy( st_cur->wpa.anonce, &h80211[z + 17], 32 );
                st_cur->wpa.state = 1;
            }


            /* frame 2 or 4: Pairwise == 1, Install == 0, Ack == 0, MIC == 1 */

            if( z+17+32 > caplen )
                goto write_packet;

            if( ( h80211[z + 6] & 0x08 ) != 0 &&
                  ( h80211[z + 6] & 0x40 ) == 0 &&
                  ( h80211[z + 6] & 0x80 ) == 0 &&
                  ( h80211[z + 5] & 0x01 ) != 0 )
            {
                if( memcmp( &h80211[z + 17], ZERO, 32 ) != 0 )
                {
                    memcpy( st_cur->wpa.snonce, &h80211[z + 17], 32 );
                    st_cur->wpa.state |= 2;

                }
            }

            /* frame 3: Pairwise == 1, Install == 1, Ack == 1, MIC == 1 */

            if( ( h80211[z + 6] & 0x08 ) != 0 &&
                  ( h80211[z + 6] & 0x40 ) != 0 &&
                  ( h80211[z + 6] & 0x80 ) != 0 &&
                  ( h80211[z + 5] & 0x01 ) != 0 )
            {
                if( memcmp( &h80211[z + 17], ZERO, 32 ) != 0 )
                {
                    memcpy( st_cur->wpa.anonce, &h80211[z + 17], 32 );
                    st_cur->wpa.state |= 1;
                }
                st_cur->wpa.eapol_size = ( h80211[z + 2] << 8 )
                        +   h80211[z + 3] + 4;

                memcpy( st_cur->wpa.keymic, &h80211[z + 81], 16 );
                memcpy( st_cur->wpa.eapol,  &h80211[z], st_cur->wpa.eapol_size );
                memset( st_cur->wpa.eapol + 81, 0, 16 );
                st_cur->wpa.state |= 4;
                st_cur->wpa.keyver = h80211[z + 6] & 7;

            }

            if( st_cur->wpa.state == 7)
            {
                memcpy( st_cur->wpa.stmac, st_cur->stmac, 6 );
                memcpy( G.wpa_bssid, ap_cur->bssid, 6 );
                memset(G.message, '\x00', sizeof(G.message));
                snprintf( G.message, sizeof( G.message ) - 1,
                    "][ WPA handshake: %02X:%02X:%02X:%02X:%02X:%02X ",
                    G.wpa_bssid[0], G.wpa_bssid[1], G.wpa_bssid[2],
                    G.wpa_bssid[3], G.wpa_bssid[4], G.wpa_bssid[5]);


                if( G.f_ivs != NULL )
                {
                    memset(&ivs2, '\x00', sizeof(struct ivs2_pkthdr));
                    ivs2.flags = 0;
                    ivs2.len = 0;

                    ivs2.len= sizeof(struct WPA_hdsk);
                    ivs2.flags |= IVS2_WPA;

                    if( memcmp( G.prev_bssid, ap_cur->bssid, 6 ) != 0 )
                    {
                        ivs2.flags |= IVS2_BSSID;
                        ivs2.len += 6;
                        memcpy( G.prev_bssid, ap_cur->bssid,  6 );
                    }

                    if( fwrite( &ivs2, 1, sizeof(struct ivs2_pkthdr), G.f_ivs )
                        != (size_t) sizeof(struct ivs2_pkthdr) )
                    {
                        perror( "fwrite(IV header) failed" );
                        return( 1 );
                    }

                    if( ivs2.flags & IVS2_BSSID )
                    {
                        if( fwrite( ap_cur->bssid, 1, 6, G.f_ivs ) != (size_t) 6 )
                        {
                            perror( "fwrite(IV bssid) failed" );
                            return( 1 );
                        }
                        ivs2.len -= 6;
                    }

                    if( fwrite( &(st_cur->wpa), 1, sizeof(struct WPA_hdsk), G.f_ivs ) != (size_t) sizeof(struct WPA_hdsk) )
                    {
                        perror( "fwrite(IV wpa_hdsk) failed" );
                        return( 1 );
                    }
                }
            }
        }
    }


write_packet:

    if(ap_cur != NULL)
    {
        if( h80211[0] == 0x80 && G.one_beacon){
            if( !ap_cur->beacon_logged )
                ap_cur->beacon_logged = 1;
            else return ( 0 );
        }
    }

    if(G.record_data)
    {
        if( ( (h80211[0] & 0x0C) == 0x00 ) && ( (h80211[0] & 0xF0) == 0xB0 ) )
        {
            /* authentication packet */
            check_shared_key(h80211, caplen);
        }
    }

    if(ap_cur != NULL)
    {
        if(ap_cur->security != 0 && G.f_encrypt != 0 && ((ap_cur->security & G.f_encrypt) == 0))
        {
            return(1);
        }
    }

    /* this changes the local ap_cur, st_cur and na_cur variables and should be the last check befor the actual write */
    if(caplen < 24 && caplen >= 10 && h80211[0])
    {
        /* RTS || CTS || ACK || CF-END || CF-END&CF-ACK*/
        //(h80211[0] == 0xB4 || h80211[0] == 0xC4 || h80211[0] == 0xD4 || h80211[0] == 0xE4 || h80211[0] == 0xF4)

        /* use general control frame detection, as the structure is always the same: mac(s) starting at [4] */
        if(h80211[0] & 0x04)
        {
            p=h80211+4;
            while(p <= h80211+16 && p<=h80211+caplen)
            {
                memcpy(namac, p, 6);

                if(memcmp(namac, NULL_MAC, 6) == 0)
                {
                    p+=6;
                    continue;
                }

                if(memcmp(namac, BROADCAST, 6) == 0)
                {
                    p+=6;
                    continue;
                }

                if(G.hide_known)
                {
                    /* check AP list */
                    ap_cur = G.ap_1st;
                    ap_prv = NULL;

                    while( ap_cur != NULL )
                    {
                        if( ! memcmp( ap_cur->bssid, namac, 6 ) )
                            break;

                        ap_prv = ap_cur;
                        ap_cur = ap_cur->next;
                    }

                    /* if it's an AP, try next mac */

                    if( ap_cur != NULL )
                    {
                        p+=6;
                        continue;
                    }

                    /* check ST list */
                    st_cur = G.st_1st;
                    st_prv = NULL;

                    while( st_cur != NULL )
                    {
                        if( ! memcmp( st_cur->stmac, namac, 6 ) )
                            break;

                        st_prv = st_cur;
                        st_cur = st_cur->next;
                    }

                    /* if it's a client, try next mac */

                    if( st_cur != NULL )
                    {
                        p+=6;
                        continue;
                    }
                }

                /* not found in either AP list or ST list, look through NA list */
                na_cur = G.na_1st;
                na_prv = NULL;

                while( na_cur != NULL )
                {
                    if( ! memcmp( na_cur->namac, namac, 6 ) )
                        break;

                    na_prv = na_cur;
                    na_cur = na_cur->next;
                }

                /* update our chained list of unknown stations */
                /* if it's a new mac, add it */

                if( na_cur == NULL )
                {
                    if( ! ( na_cur = (struct NA_info *) malloc(
                                    sizeof( struct NA_info ) ) ) )
                    {
                        perror( "malloc failed" );
                        return( 1 );
                    }

                    memset( na_cur, 0, sizeof( struct NA_info ) );

                    if( G.na_1st == NULL )
                        G.na_1st = na_cur;
                    else
                        na_prv->next  = na_cur;

                    memcpy( na_cur->namac, namac, 6 );

                    na_cur->prev = na_prv;

                    gettimeofday(&(na_cur->tv), NULL);
                    na_cur->tinit = time( NULL );
                    na_cur->tlast = time( NULL );

                    na_cur->power   = -1;
                    na_cur->channel = -1;
                    na_cur->ack     = 0;
                    na_cur->ack_old = 0;
                    na_cur->ackps   = 0;
                    na_cur->cts     = 0;
                    na_cur->rts_r   = 0;
                    na_cur->rts_t   = 0;
                }

                /* update the last time seen & power*/

                na_cur->tlast = time( NULL );
                na_cur->power = ri->ri_power;
                na_cur->channel = ri->ri_channel;

                switch(h80211[0] & 0xF0)
                {
                    case 0xB0:
                        if(p == h80211+4)
                            na_cur->rts_r++;
                        if(p == h80211+10)
                            na_cur->rts_t++;
                        break;

                    case 0xC0:
                        na_cur->cts++;
                        break;

                    case 0xD0:
                        na_cur->ack++;
                        break;

                    default:
                        na_cur->other++;
                        break;
                }

                /*grab next mac (for rts frames)*/
                p+=6;
            }
        }
    }

    if( G.f_cap != NULL && caplen >= 10)
    {
        pkh.caplen = pkh.len = caplen;

        gettimeofday( &tv, NULL );

        pkh.tv_sec  =   tv.tv_sec;
        pkh.tv_usec = ( tv.tv_usec & ~0x1ff ) + ri->ri_power + 64;

        n = sizeof( pkh );

        if( fwrite( &pkh, 1, n, G.f_cap ) != (size_t) n )
        {
            perror( "fwrite(packet header) failed" );
            return( 1 );
        }

        fflush( stdout );

        n = pkh.caplen;

        if( fwrite( h80211, 1, n, G.f_cap ) != (size_t) n )
        {
            perror( "fwrite(packet data) failed" );
            return( 1 );
        }

        fflush( stdout );
    }

    return( 0 );
}

void dump_sort_power( void )
{
    time_t tt = time( NULL );

    /* thanks to Arnaud Cornet :-) */

    struct AP_info *new_ap_1st = NULL;
    struct AP_info *new_ap_end = NULL;

    struct ST_info *new_st_1st = NULL;
    struct ST_info *new_st_end = NULL;

    struct ST_info *st_cur, *st_min;
    struct AP_info *ap_cur, *ap_min;

    /* sort the aps by power first */

    while( G.ap_1st )
    {
        ap_min = NULL;
        ap_cur = G.ap_1st;

        while( ap_cur != NULL )
        {
            if( tt - ap_cur->tlast > 20 )
                ap_min = ap_cur;

            ap_cur = ap_cur->next;
        }

        if( ap_min == NULL )
        {
            ap_min = ap_cur = G.ap_1st;

            while( ap_cur != NULL )
            {
                if( ap_cur->avg_power < ap_min->avg_power)
                    ap_min = ap_cur;

                ap_cur = ap_cur->next;
            }
        }

        if( ap_min == G.ap_1st )
            G.ap_1st = ap_min->next;

        if( ap_min == G.ap_end )
            G.ap_end = ap_min->prev;

        if( ap_min->next )
            ap_min->next->prev = ap_min->prev;

        if( ap_min->prev )
            ap_min->prev->next = ap_min->next;

        if( new_ap_end )
        {
            new_ap_end->next = ap_min;
            ap_min->prev = new_ap_end;
            new_ap_end = ap_min;
            new_ap_end->next = NULL;
        }
        else
        {
            new_ap_1st = new_ap_end = ap_min;
            ap_min->next = ap_min->prev = NULL;
        }
    }

    G.ap_1st = new_ap_1st;
    G.ap_end = new_ap_end;

    /* now sort the stations */

    while( G.st_1st )
    {
        st_min = NULL;
        st_cur = G.st_1st;

        while( st_cur != NULL )
        {
            if( tt - st_cur->tlast > 60 )
                st_min = st_cur;

            st_cur = st_cur->next;
        }

        if( st_min == NULL )
        {
            st_min = st_cur = G.st_1st;

            while( st_cur != NULL )
            {
                if( st_cur->power < st_min->power)
                    st_min = st_cur;

                st_cur = st_cur->next;
            }
        }

        if( st_min == G.st_1st )
            G.st_1st = st_min->next;

        if( st_min == G.st_end )
            G.st_end = st_min->prev;

        if( st_min->next )
            st_min->next->prev = st_min->prev;

        if( st_min->prev )
            st_min->prev->next = st_min->next;

        if( new_st_end )
        {
            new_st_end->next = st_min;
            st_min->prev = new_st_end;
            new_st_end = st_min;
            new_st_end->next = NULL;
        }
        else
        {
            new_st_1st = new_st_end = st_min;
            st_min->next = st_min->prev = NULL;
        }
    }

    G.st_1st = new_st_1st;
    G.st_end = new_st_end;
}

int getBatteryState()
{
	return get_battery_state();
}

char * getStringTimeFromSec(double seconds)
{
    int hour[3];
    char * ret;
    char * HourTime;
    char * MinTime;

    if (seconds <0)
        return NULL;

    ret = (char *) calloc(1,256);

    HourTime = (char *) calloc (1,128);
    MinTime  = (char *) calloc (1,128);

    hour[0]  = (int) (seconds);
    hour[1]  = hour[0] / 60;
    hour[2]  = hour[1] / 60;
    hour[0] %= 60 ;
    hour[1] %= 60 ;

    if (hour[2] != 0 )
        snprintf(HourTime, 128, "%d %s", hour[2], ( hour[2] == 1 ) ? "hour" : "hours");
    if (hour[1] != 0 )
        snprintf(MinTime, 128, "%d %s", hour[1], ( hour[1] == 1 ) ? "min" : "mins");

    if ( hour[2] != 0 && hour[1] != 0 )
        snprintf(ret, 256, "%s %s", HourTime, MinTime);
    else
    {
        if (hour[2] == 0 && hour[1] == 0)
            snprintf(ret, 256, "%d s", hour[0] );
        else
            snprintf(ret, 256, "%s", (hour[2] == 0) ? MinTime : HourTime );
    }

    free(MinTime);
    free(HourTime);

    return ret;

}

char * getBatteryString(void)
{
    int batt_time;
    char * ret;
    char * batt_string;

    batt_time = getBatteryState();

    if ( batt_time <= 60 ) {
        ret = (char *) calloc(1,2);
        ret[0] = ']';
        return ret;
    }

    batt_string = getStringTimeFromSec( (double) batt_time );

    ret = (char *) calloc( 1, 256 );

    snprintf( ret, 256, "][ BAT: %s ]", batt_string );

    free( batt_string);

    return ret;
}

void dump_print( int ws_row, int ws_col, int if_num )
{
    time_t tt;
    struct tm *lt;
    int nlines, i, n, len;
    char strbuf[512];
    char buffer[512];
    char ssid_list[512];
    struct AP_info *ap_cur;
    struct ST_info *st_cur;
    struct NA_info *na_cur;
    int columns_ap = 83;
    int columns_sta = 72;
    int columns_na = 68;

    if(!G.singlechan) columns_ap -= 4; //no RXQ in scan mode

    nlines = 2;

    if( nlines >= ws_row )
        return;

    tt = time( NULL );
    lt = localtime( &tt );

    if(G.is_berlin)
    {
        G.maxaps = 0;
        G.numaps = 0;
        ap_cur = G.ap_end;

        while( ap_cur != NULL )
        {
            G.maxaps++;
            if( ap_cur->nb_pkt < 2 || time( NULL ) - ap_cur->tlast > G.berlin ||
                memcmp( ap_cur->bssid, BROADCAST, 6 ) == 0 )
            {
                ap_cur = ap_cur->prev;
                continue;
            }
            G.numaps++;
            ap_cur = ap_cur->prev;
        }

        if(G.numaps > G.maxnumaps)
            G.maxnumaps = G.numaps;

        G.maxaps--;
    }

    /*
     *  display the channel, battery, position (if we are connected to GPSd)
     *  and current time
     */

    memset( strbuf, '\0', sizeof(strbuf) );
    strbuf[ws_col - 1] = '\0';
    fprintf( stderr, "%s\n", strbuf );

    snprintf(strbuf, sizeof(strbuf)-1, " CH %2d", G.channel[0]);
    for(i=1; i<if_num; i++)
    {
        memset( buffer, '\0', sizeof(buffer) );
        snprintf(buffer, sizeof(buffer) , ",%2d", G.channel[i]);
        strncat(strbuf, buffer, (sizeof(strbuf)-strlen(strbuf)));
    }

    memset( buffer, '\0', sizeof(buffer) );

    if (G.gps_loc[0]) {
        snprintf( buffer, sizeof( buffer ) - 1,
              " %s[ GPS %8.3f %8.3f %8.3f %6.2f "
              "][ Elapsed: %s ][ %04d-%02d-%02d %02d:%02d ", G.batt,
              G.gps_loc[0], G.gps_loc[1], G.gps_loc[2], G.gps_loc[3],
              G.elapsed_time , 1900 + lt->tm_year,
              1 + lt->tm_mon, lt->tm_mday, lt->tm_hour, lt->tm_min );
    }
    else
    {
        snprintf( buffer, sizeof( buffer ) - 1,
              " %s[ Elapsed: %s ][ %04d-%02d-%02d %02d:%02d ",
              G.batt, G.elapsed_time, 1900 + lt->tm_year,
              1 + lt->tm_mon, lt->tm_mday, lt->tm_hour, lt->tm_min );
    }

    strncat(strbuf, buffer, (512-strlen(strbuf)));
    memset( buffer, '\0', 512 );

    if(G.is_berlin)
    {
        snprintf( buffer, sizeof( buffer ) - 1,
              " ][%3d/%3d/%4d ",
              G.numaps, G.maxnumaps, G.maxaps);
    }

    strncat(strbuf, buffer, (512-strlen(strbuf)));
    memset( buffer, '\0', 512 );

    if(strlen(G.message) > 0)
    {
        strncat(strbuf, G.message, (512-strlen(strbuf)));
    }

    //add traling spaces to overwrite previous messages
    strncat(strbuf, "                                        ", (512-strlen(strbuf)));

    strbuf[ws_col - 1] = '\0';
    fprintf( stderr, "%s\n", strbuf );

    /* print some informations about each detected AP */

    nlines += 3;

    if( nlines >= ws_row )
        return;

    memset( strbuf, ' ', ws_col - 1 );
    strbuf[ws_col - 1] = '\0';
    fprintf( stderr, "%s\n", strbuf );

    if(G.singlechan)
    {
        memcpy( strbuf, " BSSID              PWR RXQ  Beacons"
                        "    #Data, #/s  CH  MB  ENC  CIPHER AUTH ESSID", columns_ap );
    }
    else
    {
        memcpy( strbuf, " BSSID              PWR  Beacons"
                        "    #Data, #/s  CH  MB  ENC  CIPHER AUTH ESSID", columns_ap );
    }

    strbuf[ws_col - 1] = '\0';
    fprintf( stderr, "%s\n", strbuf );

    memset( strbuf, ' ', ws_col - 1 );
    strbuf[ws_col - 1] = '\0';
    fprintf( stderr, "%s\n", strbuf );

    ap_cur = G.ap_end;

    while( ap_cur != NULL )
    {
        /* skip APs with only one packet, or those older than 2 min.
         * always skip if bssid == broadcast */

        if( ap_cur->nb_pkt < 2 || time( NULL ) - ap_cur->tlast > G.berlin ||
            memcmp( ap_cur->bssid, BROADCAST, 6 ) == 0 )
        {
            ap_cur = ap_cur->prev;
            continue;
        }

        if(ap_cur->security != 0 && G.f_encrypt != 0 && ((ap_cur->security & G.f_encrypt) == 0))
        {
            ap_cur = ap_cur->prev;
            continue;
        }

        nlines++;

        if( nlines > (ws_row-1) )
            return;

        memset(strbuf, '\0', sizeof(strbuf));

        snprintf( strbuf, sizeof(strbuf), " %02X:%02X:%02X:%02X:%02X:%02X",
                ap_cur->bssid[0], ap_cur->bssid[1],
                ap_cur->bssid[2], ap_cur->bssid[3],
                ap_cur->bssid[4], ap_cur->bssid[5] );

        len = strlen(strbuf);

        if(G.singlechan)
        {
            snprintf( strbuf+len, sizeof(strbuf)-len, "  %3d %3d %8ld %8ld %4d",
                     ap_cur->avg_power,
                     ap_cur->rx_quality,
                     ap_cur->nb_bcn,
                     ap_cur->nb_data,
                     ap_cur->nb_dataps );
        }
        else
        {
            snprintf( strbuf+len, sizeof(strbuf)-len, "  %3d %8ld %8ld %4d",
                     ap_cur->avg_power,
                     ap_cur->nb_bcn,
                     ap_cur->nb_data,
                     ap_cur->nb_dataps );
        }

        len = strlen(strbuf);

        snprintf( strbuf+len, sizeof(strbuf)-len, " %3d %3d%c ",
                 ap_cur->channel, ap_cur->max_speed,
                 ( ap_cur->preamble ) ? '.' : ' ' );

        len = strlen(strbuf);

        if( (ap_cur->security & (STD_OPN|STD_WEP|STD_WPA|STD_WPA2)) == 0) snprintf( strbuf+len, sizeof(strbuf)-len, "    " );
        else if( ap_cur->security & STD_WPA2 ) snprintf( strbuf+len, sizeof(strbuf)-len, "WPA2" );
        else if( ap_cur->security & STD_WPA  ) snprintf( strbuf+len, sizeof(strbuf)-len, "WPA " );
        else if( ap_cur->security & STD_WEP  ) snprintf( strbuf+len, sizeof(strbuf)-len, "WEP " );
        else if( ap_cur->security & STD_OPN  ) snprintf( strbuf+len, sizeof(strbuf)-len, "OPN " );

        strncat( strbuf, " ", sizeof(strbuf)-1);

        len = strlen(strbuf);

        if( (ap_cur->security & (ENC_WEP|ENC_TKIP|ENC_WRAP|ENC_CCMP|ENC_WEP104|ENC_WEP40)) == 0 ) snprintf( strbuf+len, sizeof(strbuf)-len, "       ");
        else if( ap_cur->security & ENC_CCMP   ) snprintf( strbuf+len, sizeof(strbuf)-len, "CCMP   ");
        else if( ap_cur->security & ENC_WRAP   ) snprintf( strbuf+len, sizeof(strbuf)-len, "WRAP   ");
        else if( ap_cur->security & ENC_TKIP   ) snprintf( strbuf+len, sizeof(strbuf)-len, "TKIP   ");
        else if( ap_cur->security & ENC_WEP104 ) snprintf( strbuf+len, sizeof(strbuf)-len, "WEP104 ");
        else if( ap_cur->security & ENC_WEP40  ) snprintf( strbuf+len, sizeof(strbuf)-len, "WEP40  ");
        else if( ap_cur->security & ENC_WEP    ) snprintf( strbuf+len, sizeof(strbuf)-len, "WEP    ");

        len = strlen(strbuf);

        if( (ap_cur->security & (AUTH_OPN|AUTH_PSK|AUTH_MGT)) == 0 ) snprintf( strbuf+len, sizeof(strbuf)-len, "   ");
        else if( ap_cur->security & AUTH_MGT   ) snprintf( strbuf+len, sizeof(strbuf)-len, "MGT");
        else if( ap_cur->security & AUTH_PSK   )
        {
            if( ap_cur->security & STD_WEP )
                snprintf( strbuf+len, sizeof(strbuf)-len, "SKA");
            else
                snprintf( strbuf+len, sizeof(strbuf)-len, "PSK");
        }
        else if( ap_cur->security & AUTH_OPN   ) snprintf( strbuf+len, sizeof(strbuf)-len, "OPN");

        len = strlen(strbuf);

        strbuf[ws_col-1] = '\0';

        fprintf(stderr, strbuf);

        if( ws_col > (columns_ap - 4) )
        {
            memset( strbuf, 0, sizeof( strbuf ) );
            if(ap_cur->essid[0] != 0x00)
            {
                snprintf( strbuf,  sizeof( strbuf ) - 1,
                          "%-256s", ap_cur->essid );
            }
            else
            {
                snprintf( strbuf,  sizeof( strbuf ) - 1,
                          "<length:%3d>%-256s", ap_cur->ssid_length, "\x00" );
            }
            strbuf[ws_col - (columns_ap - 4)] = '\0';
            fprintf( stderr, "  %s", strbuf );
        }

        fprintf( stderr, "\n" );

        ap_cur = ap_cur->prev;
    }

    /* print some informations about each detected station */

    nlines += 3;

    if( nlines >= (ws_row-1) )
        return;

    memset( strbuf, ' ', ws_col - 1 );
    strbuf[ws_col - 1] = '\0';
    fprintf( stderr, "%s\n", strbuf );

    memcpy( strbuf, " BSSID              STATION "
            "           PWR   Rate  Lost  Packets  Probes", columns_sta );
    strbuf[ws_col - 1] = '\0';
    fprintf( stderr, "%s\n", strbuf );

    memset( strbuf, ' ', ws_col - 1 );
    strbuf[ws_col - 1] = '\0';
    fprintf( stderr, "%s\n", strbuf );

    ap_cur = G.ap_end;

    while( ap_cur != NULL )
    {
        if( ap_cur->nb_pkt < 2 ||
            time( NULL ) - ap_cur->tlast > G.berlin )
        {
            ap_cur = ap_cur->prev;
            continue;
        }

        if(ap_cur->security != 0 && G.f_encrypt != 0 && ((ap_cur->security & G.f_encrypt) == 0))
        {
            ap_cur = ap_cur->prev;
            continue;
        }

        if( nlines >= (ws_row-1) )
            return;

        st_cur = G.st_end;

        while( st_cur != NULL )
        {
            if( st_cur->base != ap_cur ||
                time( NULL ) - st_cur->tlast > G.berlin )
            {
                st_cur = st_cur->prev;
                continue;
            }

            if( ! memcmp( ap_cur->bssid, BROADCAST, 6 ) && G.asso_client )
            {
                st_cur = st_cur->prev;
                continue;
            }

            nlines++;

            if( ws_row != 0 && nlines >= ws_row )
                return;

            if( ! memcmp( ap_cur->bssid, BROADCAST, 6 ) )
                fprintf( stderr, " (not associated) " );
            else
                fprintf( stderr, " %02X:%02X:%02X:%02X:%02X:%02X",
                        ap_cur->bssid[0], ap_cur->bssid[1],
                        ap_cur->bssid[2], ap_cur->bssid[3],
                        ap_cur->bssid[4], ap_cur->bssid[5] );

            fprintf( stderr, "  %02X:%02X:%02X:%02X:%02X:%02X",
                    st_cur->stmac[0], st_cur->stmac[1],
                    st_cur->stmac[2], st_cur->stmac[3],
                    st_cur->stmac[4], st_cur->stmac[5] );

            fprintf( stderr, "  %3d", st_cur->power    );
            fprintf( stderr, "  %2d", st_cur->rate_to/1000000  );
            fprintf( stderr,  "-%2d", st_cur->rate_from/1000000);
            fprintf( stderr, "  %4d", st_cur->missed   );
            fprintf( stderr, " %8ld", st_cur->nb_pkt   );

            if( ws_col > (columns_sta - 6) )
            {
                memset( ssid_list, 0, sizeof( ssid_list ) );

                for( i = 0, n = 0; i < NB_PRB; i++ )
                {
                    if( st_cur->probes[i][0] == '\0' )
                        continue;

                    snprintf( ssid_list + n, sizeof( ssid_list ) - n - 1,
                              "%c%s", ( i > 0 ) ? ',' : ' ',
                              st_cur->probes[i] );

                    n += ( 1 + strlen( st_cur->probes[i] ) );

                    if( n >= (int) sizeof( ssid_list ) )
                        break;
                }

                memset( strbuf, 0, sizeof( strbuf ) );
                snprintf( strbuf,  sizeof( strbuf ) - 1,
                          "%-256s", ssid_list );
                strbuf[ws_col - (columns_sta - 6)] = '\0';
                fprintf( stderr, " %s", strbuf );
            }

            fprintf( stderr, "\n" );

            st_cur = st_cur->prev;
        }

        ap_cur = ap_cur->prev;
    }

    if(G.show_ack)
    {
        /* print some informations about each unknown station */

        nlines += 3;

        if( nlines >= (ws_row-1) )
            return;

        memset( strbuf, ' ', ws_col - 1 );
        strbuf[ws_col - 1] = '\0';
        fprintf( stderr, "%s\n", strbuf );

        memcpy( strbuf, " MAC       "
                "         CH PWR    ACK ACK/s    CTS RTS_RX RTS_TX  OTHER", columns_na );
        strbuf[ws_col - 1] = '\0';
        fprintf( stderr, "%s\n", strbuf );

        memset( strbuf, ' ', ws_col - 1 );
        strbuf[ws_col - 1] = '\0';
        fprintf( stderr, "%s\n", strbuf );

        na_cur = G.na_1st;

        while( na_cur != NULL )
        {
            if( time( NULL ) - na_cur->tlast > 120 )
            {
                na_cur = na_cur->next;
                continue;
            }

            if( nlines >= (ws_row-1) )
                return;

            nlines++;

            if( ws_row != 0 && nlines >= ws_row )
                return;

            fprintf( stderr, " %02X:%02X:%02X:%02X:%02X:%02X",
                    na_cur->namac[0], na_cur->namac[1],
                    na_cur->namac[2], na_cur->namac[3],
                    na_cur->namac[4], na_cur->namac[5] );

            fprintf( stderr, "  %2d", na_cur->channel  );
            fprintf( stderr, " %3d", na_cur->power  );
            fprintf( stderr, " %6d", na_cur->ack );
            fprintf( stderr, "  %4d", na_cur->ackps );
            fprintf( stderr, " %6d", na_cur->cts );
            fprintf( stderr, " %6d", na_cur->rts_r );
            fprintf( stderr, " %6d", na_cur->rts_t );
            fprintf( stderr, " %6d", na_cur->other );

            fprintf( stderr, "\n" );

            na_cur = na_cur->next;
        }
    }
}

int dump_write_csv( void )
{
    int i, j, n;
    struct tm *ltime;
    char ssid_list[512];
    struct AP_info *ap_cur;
    struct ST_info *st_cur;

    if (! G.record_data)
    	return 0;

    fseek( G.f_txt, 0, SEEK_SET );

    fprintf( G.f_txt,
        "\r\nBSSID, First time seen, Last time seen, channel, Speed, "
        "Privacy, Cipher, Authentication, Power, # beacons, # IV, LAN IP, ID-length, ESSID, Key\r\n" );

    ap_cur = G.ap_1st;

    while( ap_cur != NULL )
    {
        if( memcmp( ap_cur->bssid, BROADCAST, 6 ) == 0 )
        {
            ap_cur = ap_cur->next;
            continue;
        }

        if(ap_cur->security != 0 && G.f_encrypt != 0 && ((ap_cur->security & G.f_encrypt) == 0))
        {
            ap_cur = ap_cur->next;
            continue;
        }

        if( ap_cur->nb_pkt < 2 )
        {
            ap_cur = ap_cur->next;
            continue;
        }

        fprintf( G.f_txt, "%02X:%02X:%02X:%02X:%02X:%02X, ",
                 ap_cur->bssid[0], ap_cur->bssid[1],
                 ap_cur->bssid[2], ap_cur->bssid[3],
                 ap_cur->bssid[4], ap_cur->bssid[5] );

        ltime = localtime( &ap_cur->tinit );

        fprintf( G.f_txt, "%04d-%02d-%02d %02d:%02d:%02d, ",
                 1900 + ltime->tm_year, 1 + ltime->tm_mon,
                 ltime->tm_mday, ltime->tm_hour,
                 ltime->tm_min,  ltime->tm_sec );

        ltime = localtime( &ap_cur->tlast );

        fprintf( G.f_txt, "%04d-%02d-%02d %02d:%02d:%02d, ",
                 1900 + ltime->tm_year, 1 + ltime->tm_mon,
                 ltime->tm_mday, ltime->tm_hour,
                 ltime->tm_min,  ltime->tm_sec );

        fprintf( G.f_txt, "%2d, %3d, ",
                 ap_cur->channel,
                 ap_cur->max_speed );

        if( (ap_cur->security & (STD_OPN|STD_WEP|STD_WPA|STD_WPA2)) == 0) fprintf( G.f_txt, "    " );
        else
        {
            if( ap_cur->security & STD_WPA2 ) fprintf( G.f_txt, "WPA2" );
            if( ap_cur->security & STD_WPA  ) fprintf( G.f_txt, "WPA " );
            if( ap_cur->security & STD_WEP  ) fprintf( G.f_txt, "WEP " );
            if( ap_cur->security & STD_OPN  ) fprintf( G.f_txt, "OPN " );
        }

        fprintf( G.f_txt, ",");

        if( (ap_cur->security & (ENC_WEP|ENC_TKIP|ENC_WRAP|ENC_CCMP|ENC_WEP104|ENC_WEP40)) == 0 ) fprintf( G.f_txt, "       ");
        else
        {
            if( ap_cur->security & ENC_CCMP   ) fprintf( G.f_txt, " CCMP");
            if( ap_cur->security & ENC_WRAP   ) fprintf( G.f_txt, " WRAP");
            if( ap_cur->security & ENC_TKIP   ) fprintf( G.f_txt, " TKIP");
            if( ap_cur->security & ENC_WEP104 ) fprintf( G.f_txt, " WEP104");
            if( ap_cur->security & ENC_WEP40  ) fprintf( G.f_txt, " WEP40");
            if( ap_cur->security & ENC_WEP    ) fprintf( G.f_txt, " WEP");
        }

        fprintf( G.f_txt, ",");

        if( (ap_cur->security & (AUTH_OPN|AUTH_PSK|AUTH_MGT)) == 0 ) fprintf( G.f_txt, "   ");
        else
        {
            if( ap_cur->security & AUTH_MGT   ) fprintf( G.f_txt, " MGT");
            if( ap_cur->security & AUTH_PSK   )
			{
				if( ap_cur->security & STD_WEP )
					fprintf( G.f_txt, "SKA");
				else
					fprintf( G.f_txt, "PSK");
			}
            if( ap_cur->security & AUTH_OPN   ) fprintf( G.f_txt, " OPN");
        }

        fprintf( G.f_txt, ", %3d, %8ld, %8ld, ",
                 ap_cur->avg_power,
                 ap_cur->nb_bcn,
                 ap_cur->nb_data );

        fprintf( G.f_txt, "%3d.%3d.%3d.%3d, ",
                 ap_cur->lanip[0], ap_cur->lanip[1],
                 ap_cur->lanip[2], ap_cur->lanip[3] );

        fprintf( G.f_txt, "%3d, ", ap_cur->ssid_length);

        for(i=0; i<ap_cur->ssid_length; i++)
        {
            fprintf( G.f_txt, "%c", ap_cur->essid[i] );
        }
        fprintf( G.f_txt, ", " );


        if(ap_cur->key != NULL)
        {
            for(i=0; i<(int)strlen(ap_cur->key); i++)
            {
                fprintf( G.f_txt, "%02X", ap_cur->key[i]);
                if(i<(int)(strlen(ap_cur->key)-1))
                    fprintf( G.f_txt, ":");
            }
        }

        fprintf( G.f_txt, "\r\n");

        ap_cur = ap_cur->next;
    }

    fprintf( G.f_txt,
        "\r\nStation MAC, First time seen, Last time seen, "
        "Power, # packets, BSSID, Probed ESSIDs\r\n" );

    st_cur = G.st_1st;

    while( st_cur != NULL )
    {
        ap_cur = st_cur->base;

        if( ap_cur->nb_pkt < 2 )
        {
            st_cur = st_cur->next;
            continue;
        }

        fprintf( G.f_txt, "%02X:%02X:%02X:%02X:%02X:%02X, ",
                 st_cur->stmac[0], st_cur->stmac[1],
                 st_cur->stmac[2], st_cur->stmac[3],
                 st_cur->stmac[4], st_cur->stmac[5] );

        ltime = localtime( &st_cur->tinit );

        fprintf( G.f_txt, "%04d-%02d-%02d %02d:%02d:%02d, ",
                 1900 + ltime->tm_year, 1 + ltime->tm_mon,
                 ltime->tm_mday, ltime->tm_hour,
                 ltime->tm_min,  ltime->tm_sec );

        ltime = localtime( &st_cur->tlast );

        fprintf( G.f_txt, "%04d-%02d-%02d %02d:%02d:%02d, ",
                 1900 + ltime->tm_year, 1 + ltime->tm_mon,
                 ltime->tm_mday, ltime->tm_hour,
                 ltime->tm_min,  ltime->tm_sec );

        fprintf( G.f_txt, "%3d, %8ld, ",
                 st_cur->power,
                 st_cur->nb_pkt );

        if( ! memcmp( ap_cur->bssid, BROADCAST, 6 ) )
            fprintf( G.f_txt, "(not associated) ," );
        else
            fprintf( G.f_txt, "%02X:%02X:%02X:%02X:%02X:%02X,",
                     ap_cur->bssid[0], ap_cur->bssid[1],
                     ap_cur->bssid[2], ap_cur->bssid[3],
                     ap_cur->bssid[4], ap_cur->bssid[5] );

        memset( ssid_list, 0, sizeof( ssid_list ) );

        for( i = 0, n = 0; i < NB_PRB; i++ )
        {
            if( st_cur->probes[i][0] == '\0' )
                continue;

            snprintf( ssid_list + n, sizeof( ssid_list ) - n - 1,
                      "%c", ( i > 0 ) ? ',' : ' ' );

            for(j=0; j<st_cur->ssid_length[i]; j++)
            {
                snprintf( ssid_list + n + 1 + j, sizeof( ssid_list ) - n - 2 - j,
                          "%c", st_cur->probes[i][j]);
            }

            n += ( 1 + st_cur->ssid_length[i] );
            if( n >= (int) sizeof( ssid_list ) )
                break;
        }

        fprintf( G.f_txt, "%s\r\n", ssid_list );

        st_cur = st_cur->next;
    }

    fprintf( G.f_txt, "\r\n" );
    fflush( G.f_txt );
    return 0;
}

void gps_tracker( void )
{
    int gpsd_sock;
    char line[256], *p;
    struct sockaddr_in gpsd_addr;
    int ret;

    /* attempt to connect to localhost, port 2947 */

    gpsd_sock = socket( AF_INET, SOCK_STREAM, 0 );

    if( gpsd_sock < 0 ) {
        return;
    }

    gpsd_addr.sin_family      = AF_INET;
    gpsd_addr.sin_port        = htons( 2947 );
    gpsd_addr.sin_addr.s_addr = inet_addr( "127.0.0.1" );

    if( connect( gpsd_sock, (struct sockaddr *) &gpsd_addr,
                 sizeof( gpsd_addr ) ) < 0 ) {
        return;
    }

    /* loop reading the GPS coordinates */

    while( 1 )
    {
        sleep( 1 );

        memset( G.gps_loc, 0, sizeof( float ) * 5 );

        /* read position, speed, heading, altitude */

        memset( line, 0, sizeof( line ) );
        snprintf( line,  sizeof( line ) - 1, "PVTAD\r\n" );
        if( send( gpsd_sock, line, 7, 0 ) != 7 )
            return;

        memset( line, 0, sizeof( line ) );
        if( recv( gpsd_sock, line, sizeof( line ) - 1, 0 ) <= 0 )
            return;

        if( memcmp( line, "GPSD,P=", 7 ) != 0 )
            continue;

        /* make sure the coordinates are present */

        if( line[7] == '?' )
            continue;

        ret = sscanf( line + 7, "%f %f", &G.gps_loc[0], &G.gps_loc[1] );

        if( ( p = strstr( line, "V=" ) ) == NULL ) continue;
        ret = sscanf( p + 2, "%f", &G.gps_loc[2] ); /* speed */

        if( ( p = strstr( line, "T=" ) ) == NULL ) continue;
        ret = sscanf( p + 2, "%f", &G.gps_loc[3] ); /* heading */

        if( ( p = strstr( line, "A=" ) ) == NULL ) continue;
        ret = sscanf( p + 2, "%f", &G.gps_loc[4] ); /* altitude */

        if (G.record_data)
        	fputs( line, G.f_gps );

        G.save_gps = 1;

        write( G.gc_pipe[1], G.gps_loc, sizeof( float ) * 5 );
        kill( getppid(), SIGUSR2 );
    }
}

void sighandler( int signum)
{
    int card=0;

    signal( signum, sighandler );

    if( signum == SIGUSR1 )
    {
	read( G.cd_pipe[0], &card, sizeof(int) );
        read( G.ch_pipe[0], &(G.channel[card]), sizeof( int ) );
    }

    if( signum == SIGUSR2 )
        read( G.gc_pipe[0], &G.gps_loc, sizeof( float ) * 5 );

    if( signum == SIGINT || signum == SIGTERM )
    {
        alarm( 1 );
        G.do_exit = 1;
        signal( SIGALRM, sighandler );
        printf( "\n" );
    }

    if( signum == SIGSEGV )
    {
        fprintf( stderr, "Caught signal 11 (SIGSEGV). Please"
                         " contact the author!\33[?25h\n\n" );
        fflush( stdout );
        exit( 1 );
    }

    if( signum == SIGALRM )
    {
        fprintf( stderr, "Caught signal 14 (SIGALRM). Please"
                         " contact the author!\33[?25h\n\n" );
        fflush( stdout );
        exit( 1 );
    }

    if( signum == SIGCHLD )
        wait( NULL );

    if( signum == SIGWINCH )
    {
        fprintf( stderr, "\33[2J" );
        fflush( stdout );
    }
}

int getchancount(int valid)
{
    int i=0, chan_count=0;

    while(G.channels[i])
    {
        i++;
        if(G.channels[i] != -1)
            chan_count++;
    }

    if(valid) return chan_count;
    return i;
}

void channel_hopper(struct wif *wi[], int if_num, int chan_count )
{
    int ch, ch_idx = 0, card=0, chi=0, cai=0, j=0, k=0, first=1, again=1;
    int dropped=0;

    while( getppid() != 1 )
    {
        for( j = 0; j < if_num; j++ )
        {
            again = 1;

            ch_idx = chi % chan_count;

            card = cai % if_num;

            ++chi;
            ++cai;

            if( G.chswitch == 2 && !first )
            {
                j = if_num - 1;
                card = if_num - 1;

                if( getchancount(1) > if_num )
                {
                    while( again )
                    {
                        again = 0;
                        for( k = 0; k < ( if_num - 1 ); k++ )
                        {
                            if( G.channels[ch_idx] == G.channel[k] )
                            {
                                again = 1;
                                ch_idx = chi % chan_count;
                                chi++;
                            }
                        }
                    }
                }
            }

            if( G.channels[ch_idx] == -1 )
            {
                j--;
                cai--;
                dropped++;
                if(dropped >= chan_count)
                {
                    ch = wi_get_channel(wi[card]);
                    G.channel[card] = ch;
                    write( G.cd_pipe[1], &card, sizeof(int) );
                    write( G.ch_pipe[1], &ch, sizeof( int ) );
                    kill( getppid(), SIGUSR1 );
                    usleep(1000);
                }
                continue;
            }

            dropped = 0;

            ch = G.channels[ch_idx];

            if(wi_set_channel(wi[card], ch ) == 0 )
            {
                G.channel[card] = ch;
                write( G.cd_pipe[1], &card, sizeof(int) );
                write( G.ch_pipe[1], &ch, sizeof( int ) );
                kill( getppid(), SIGUSR1 );
                usleep(1000);
            }
            else
            {
                G.channels[ch_idx] = -1;      /* remove invalid channel */
                j--;
                cai--;
                continue;
            }
        }

        if(G.chswitch == 0)
        {
            chi=chi-(if_num - 1);
        }

        if(first)
        {
            first = 0;
        }

        usleep( (G.hopfreq*1000) );
    }

    exit( 0 );
}

int invalid_channel(int chan)
{
    int i=0;

    do
    {
        if (chan == abg_chans[i] && chan != 0 )
            return 0;
    } while (abg_chans[++i]);
    return 1;
}

/* parse a string, for example "1,2,3-7,11" */

int getchannels(const char *optarg)
{
    unsigned int i=0,chan_cur=0,chan_first=0,chan_last=0,chan_max=128,chan_remain=0;
    char *optchan = NULL, *optc;
    char *token = NULL;
    int *tmp_channels;

    //got a NULL pointer?
    if(optarg == NULL)
        return -1;

    chan_remain=chan_max;

    //create a writable string
    optc = optchan = (char*) malloc(strlen(optarg)+1);
    strncpy(optchan, optarg, strlen(optarg));
    optchan[strlen(optarg)]='\0';

    tmp_channels = (int*) malloc(sizeof(int)*(chan_max+1));

    //split string in tokens, separated by ','
    while( (token = strsep(&optchan,",")) != NULL)
    {
        //range defined?
        if(strchr(token, '-') != NULL)
        {
            //only 1 '-' ?
            if(strchr(token, '-') == strrchr(token, '-'))
            {
                //are there any illegal characters?
                for(i=0; i<strlen(token); i++)
                {
                    if( (token[i] < '0') && (token[i] > '9') && (token[i] != '-'))
                    {
                        free(tmp_channels);
                        free(optc);
                        return -1;
                    }
                }

                if( sscanf(token, "%d-%d", &chan_first, &chan_last) != EOF )
                {
                    if(chan_first > chan_last)
                    {
                        free(tmp_channels);
                        free(optc);
                        return -1;
                    }
                    for(i=chan_first; i<=chan_last; i++)
                    {
                        if( (! invalid_channel(i)) && (chan_remain > 0) )
                        {
                                tmp_channels[chan_max-chan_remain]=i;
                                chan_remain--;
                        }
                    }
                }
                else
                {
                    free(tmp_channels);
                    free(optc);
                    return -1;
                }

            }
            else
            {
                free(tmp_channels);
                free(optc);
                return -1;
            }
        }
        else
        {
            //are there any illegal characters?
            for(i=0; i<strlen(token); i++)
            {
                if( (token[i] < '0') && (token[i] > '9') )
                {
                    free(tmp_channels);
                    free(optc);
                    return -1;
                }
            }

            if( sscanf(token, "%d", &chan_cur) != EOF)
            {
                if( (! invalid_channel(chan_cur)) && (chan_remain > 0) )
                {
                        tmp_channels[chan_max-chan_remain]=chan_cur;
                        chan_remain--;
                }

            }
            else
            {
                free(tmp_channels);
                free(optc);
                return -1;
            }
        }
    }

    G.own_channels = (int*) malloc(sizeof(int)*(chan_max - chan_remain + 1));

    for(i=0; i<(chan_max - chan_remain); i++)
    {
        G.own_channels[i]=tmp_channels[i];
    }

    G.own_channels[i]=0;

    free(tmp_channels);
    free(optc);
    if(i==1) return G.own_channels[0];
    if(i==0) return -1;
    return 0;
}

int setup_card(char *iface, struct wif **wis)
{
	struct wif *wi;

	wi = wi_open(iface);
	if (!wi)
		return -1;
	*wis = wi;

	return 0;
}

int init_cards(const char* cardstr, char *iface[], struct wif **wi)
{
    char *buffer;
    char *buf;
    int if_count=0;
    int i=0, again=0;

    buf = buffer = (char*) malloc( sizeof(char) * 1025 );
    strncpy( buffer, cardstr, 1025 );
    buffer[1024] = '\0';

    while( ((iface[if_count]=strsep(&buffer, ",")) != NULL) && (if_count < MAX_CARDS) )
    {
        again=0;
        for(i=0; i<if_count; i++)
        {
            if(strcmp(iface[i], iface[if_count]) == 0)
            again=1;
        }
        if(again) continue;
        if(setup_card(iface[if_count], &(wi[if_count])) != 0)
        {
            free(buf);
            return -1;
        }
        if_count++;
    }

    free(buf);
    return if_count;
}

int get_if_num(const char* cardstr)
{
    char *buffer;
    int if_count=0;

    buffer = (char*) malloc(sizeof(char)*1025);
    strncpy(buffer, cardstr, 1025);
    buffer[1024] = '\0';

    while( (strsep(&buffer, ",") != NULL) && (if_count < MAX_CARDS) )
    {
        if_count++;
    }

    return if_count;
}

int set_encryption_filter(const char* input)
{
    if(input == NULL) return 1;

    if(strlen(input) < 3) return 1;

    if(strcasecmp(input, "opn") == 0)
        G.f_encrypt |= STD_OPN;

    if(strcasecmp(input, "wep") == 0)
        G.f_encrypt |= STD_WEP;

    if(strcasecmp(input, "wpa") == 0)
    {
        G.f_encrypt |= STD_WPA;
        G.f_encrypt |= STD_WPA2;
    }

    if(strcasecmp(input, "wpa1") == 0)
        G.f_encrypt |= STD_WPA;

    if(strcasecmp(input, "wpa2") == 0)
        G.f_encrypt |= STD_WPA2;

    return 0;
}

int check_monitor(struct wif *wi[], int *fd_raw, int *fdh, int cards)
{
    int i, monitor;
    char ifname[64];

    for(i=0; i<cards; i++)
    {
        monitor = wi_get_monitor(wi[i]);
        if(monitor != 0)
        {
            memset(G.message, '\x00', sizeof(G.message));
            snprintf(G.message, sizeof(G.message), "][ %s reset to monitor mode", wi_get_ifname(wi[i]));
            //reopen in monitor mode

            strncpy(ifname, wi_get_ifname(wi[i]), sizeof(ifname)-1);
            ifname[sizeof(ifname)-1] = 0;

            wi_close(wi[i]);
            wi[i] = wi_open(ifname);
            if (!wi[i]) {
                printf("Can't reopen %s\n", ifname);
                exit(1);
            }

            fd_raw[i] = wi_fd(wi[i]);
            if (fd_raw[i] > *fdh)
                *fdh = fd_raw[i];
        }
    }
    return 0;
}

int check_channel(struct wif *wi[], int cards)
{
    int i, chan;
    for(i=0; i<cards; i++)
    {
        chan = wi_get_channel(wi[i]);
        if(G.channel[i] != chan)
        {
            memset(G.message, '\x00', sizeof(G.message));
            snprintf(G.message, sizeof(G.message), "][ fixed channel %s: %d ", wi_get_ifname(wi[i]), chan);
            wi_set_channel(wi[i], G.channel[i]);
        }
    }
    return 0;
}

int main( int argc, char *argv[] )
{
    long time_slept, cycle_time;
    int caplen=0, i, j, cards, fdh, fd_is_set, chan_count;
    int fd_raw[MAX_CARDS], arptype[MAX_CARDS];
    int ivs_only, found;
    int valid_channel, chanoption;
    int freq [2];
    int num_opts = 0;
    int option = 0;
    int option_index = 0;
    char ifnam[64];
    int wi_read_failed=0;
    int n = 0;

    struct AP_info *ap_cur, *ap_prv, *ap_next;
    struct ST_info *st_cur, *st_next;
    struct NA_info *na_cur, *na_next;

    struct pcap_pkthdr pkh;

    time_t tt1, tt2, tt3, start_time;

    struct wif	       *wi[MAX_CARDS];
    struct rx_info     ri;
    unsigned char      tmpbuf[4096];
    unsigned char      buffer[4096];
    unsigned char      *h80211;
    char               *iface[MAX_CARDS];

    struct timeval     tv0;
    struct timeval     tv1;
    struct timeval     tv2;
    struct timeval     tv3;
    struct winsize     ws;
    struct tm          *lt;

    /*
    struct sockaddr_in provis_addr;
    */

    fd_set             rfds;

    /* initialize a bunch of variables */

    memset( &G, 0, sizeof( G ) );

    h80211         =  NULL;
    ivs_only       =  0;
    chanoption     =  0;
    cards	   =  0;
    fdh		   =  0;
    fd_is_set	   =  0;
    chan_count	   =  0;
    time_slept     =  0;
    G.batt         =  NULL;
    G.chswitch     =  0;
    valid_channel  =  0;
    G.usegpsd      =  0;
    G.channels     =  bg_chans;
    G.one_beacon   =  1;
    G.singlechan   =  0;
    G.dump_prefix  =  NULL;
    G.record_data  =  0;
    G.f_cap        =  NULL;
    G.f_ivs        =  NULL;
    G.f_txt        =  NULL;
    G.f_gps        =  NULL;
    G.keyout       =  NULL;
    G.f_xor        =  NULL;
    G.sk_len       =  0;
    G.sk_start     =  0;
    G.prefix       =  NULL;
    G.f_encrypt    =  0;
    G.asso_client  =  0;
    G.update_s     =  0;
    G.decloak      =  1;
    G.is_berlin    =  0;
    G.numaps       =  0;
    G.maxnumaps    =  0;
    G.berlin       =  120;
    G.show_ack     =  0;
    G.hide_known   =  0;
    G.hopfreq      =  350;
    G.s_file       =  NULL;
    G.s_iface      =  NULL;
    G.f_cap_in     =  NULL;
    G.detect_anomaly = 0;

    memset(G.sharedkey, '\x00', 512*3);
    memset(G.message, '\x00', sizeof(G.message));
    memset(&G.pfh_in, '\x00', sizeof(struct pcap_file_header));

    gettimeofday( &tv0, NULL );

    lt = localtime( (time_t *) &tv0.tv_sec );

    G.keyout = (char*) malloc(512);
    memset( G.keyout, 0, 512 );
    snprintf( G.keyout,  511,
              "keyout-%02d%02d-%02d%02d%02d.keys",
              lt->tm_mon + 1, lt->tm_mday,
              lt->tm_hour, lt->tm_min, lt->tm_sec );

    for(i=0; i<MAX_CARDS; i++)
    {
        arptype[i]=0;
        fd_raw[i]=-1;
        G.channel[i]=0;
    }

    memset(G.f_bssid, '\x00', 6);
    memset(G.f_netmask, '\x00', 6);
    memset(G.wpa_bssid, '\x00', 6);


    /* check the arguments */
    static struct option long_options[] = {
        {"band",     1, 0, 'b'},
        {"beacon",   0, 0, 'e'},
        {"beacons",  0, 0, 'e'},
        {"cswitch",  1, 0, 's'},
        {"netmask",  1, 0, 'm'},
        {"bssid",    1, 0, 'd'},
        {"channel",  1, 0, 'c'},
        {"gpsd",     0, 0, 'g'},
        {"ivs",      0, 0, 'i'},
        {"write",    1, 0, 'w'},
        {"encrypt",  1, 0, 't'},
        {"update",   1, 0, 'u'},
        {"berlin",   1, 0, 'B'},
        {"help",     0, 0, 'H'},
        {"nodecloak",0, 0, 'D'},
        {"showack",  0, 0, 'A'},
        {"detect-anomaly", 0, 0, 'E'},
        {0,          0, 0,  0 }
    };

    for(i=0; long_options[i].name != NULL; i++);
    num_opts = i;

    for(i=0; i<argc; i++) //go through all arguments
    {
        found = 0;
        if(strlen(argv[i]) >= 3)
        {
            if(argv[i][0] == '-' && argv[i][1] != '-')
            {
                //we got a single dash followed by at least 2 chars
                //lets check that against our long options to find errors
                for(j=0; j<num_opts;j++)
                {
                    if( strcmp(argv[i]+1, long_options[j].name) == 0 )
                    {
                        //found long option after single dash
                        found = 1;
                        if(i>1 && strcmp(argv[i-1], "-") == 0)
                        {
                            //separated dashes?
                            printf("Notice: You specified \"%s %s\". Did you mean \"%s%s\" instead?\n", argv[i-1], argv[i], argv[i-1], argv[i]);
                        }
                        else
                        {
                            //forgot second dash?
                            printf("Notice: You specified \"%s\". Did you mean \"-%s\" instead?\n", argv[i], argv[i]);
                        }
                        break;
                    }
                }
                if(found)
                {
                    sleep(3);
                    break;
                }
            }
        }
    }

    do
    {
        option_index = 0;

        option = getopt_long( argc, argv,
                        "b:c:egiw:s:t:u:m:d:aHDB:Ahf:r:E",
                        long_options, &option_index );

        if( option < 0 ) break;

        switch( option )
        {
            case 0 :

                break;

            case ':':

                printf("\"%s --help\" for help.\n", argv[0]);
                return( 1 );

            case '?':

                printf("\"%s --help\" for help.\n", argv[0]);
                return( 1 );

			case 'E':
				G.detect_anomaly = 1;
				break;

            case 'e':

                G.one_beacon = 0;
                break;

            case 'a':

                G.asso_client = 1;
                break;

            case 'A':

                G.show_ack = 1;
                break;

            case 'h':

                G.hide_known = 1;
                break;

            case 'D':

                G.decloak = 0;
                break;

            case 'c' :

                if (G.channel[0] > 0 || chanoption == 1) {
                    if (chanoption == 1)
                        printf( "Notice: Channel range already given\n" );
                    else
                        printf( "Notice: Channel already given (%d)\n", G.channel[0]);
                    break;
                }

                G.channel[0] = getchannels(optarg);

                if ( G.channel[0] < 0 )
                    goto usage;

                chanoption = 1;

                if( G.channel[0] == 0 )
                {
                    G.channels = G.own_channels;
                    break;
                }
                G.channels = bg_chans;
                break;

            case 'b' :

                if (chanoption == 1 && option != 'c') {
                    printf( "Notice: Channel range already given\n" );
                    break;
                }
                freq[0] = freq[1] = 0;

                for (i = 0; i < (int)strlen(optarg); i++) {
                    if ( optarg[i] == 'a' )
                        freq[1] = 1;
                    else if ( optarg[i] == 'b' || optarg[i] == 'g')
                        freq[0] = 1;
                    else {
                        printf( "Error: invalid band (%c)\n", optarg[i] );
                        printf("\"%s --help\" for help.\n", argv[0]);
                        exit ( 1 );
                    }
                }

                if (freq[1] + freq[0] == 2 )
                    G.channels = abg_chans;
                else {
                    if ( freq[1] == 1 )
                        G.channels = a_chans;
                    else
                        G.channels = bg_chans;
                }

                break;

            case 'i':

                ivs_only = 1;
                break;

            case 'g':

                G.usegpsd  = 1;
                /*
                if (inet_aton(optarg, &provis_addr.sin_addr) == 0 )
                {
                    printf("Invalid IP address.\n");
                    return (1);
                }
                */
                break;

            case 'w':

                if (G.dump_prefix != NULL) {
                    printf( "Notice: dump prefix already given\n" );
                    break;
                }
                /* Write prefix */
                G.dump_prefix   = optarg;
                G.record_data = 1;
                break;

            case 'r' :

                if( G.s_file )
                {
                    printf( "Packet source already specified.\n" );
                    printf("\"%s --help\" for help.\n", argv[0]);
                    return( 1 );
                }
                G.s_file = optarg;
                break;

            case 's':

                if (atoi(optarg) > 2) {
                    goto usage;
                }
                if (G.chswitch != 0) {
                    printf("Notice: switching method already given\n");
                    break;
                }
                G.chswitch = atoi(optarg);
                break;

            case 'u':

                G.update_s = atoi(optarg);

                /* If failed to parse or value <= 0, use default, 100ms */
                if (G.update_s <= 0)
                	G.update_s = REFRESH_RATE;

                break;

            case 'f':

                G.hopfreq = atoi(optarg);

                /* If failed to parse or value <= 0, use default, 100ms */
                if (G.hopfreq <= 0)
                	G.hopfreq = REFRESH_RATE;

                break;

            case 'B':

                G.is_berlin = 1;
                G.berlin    = atoi(optarg);

                if (G.berlin <= 0)
                	G.berlin = 120;

                break;

            case 'm':

                if ( memcmp(G.f_netmask, NULL_MAC, 6) != 0 )
                {
                    printf("Notice: netmask already given\n");
                    break;
                }
                if(getmac(optarg, 1, G.f_netmask) != 0)
                {
                    printf("Notice: invalid netmask\n");
                    printf("\"%s --help\" for help.\n", argv[0]);
                    return( 1 );
                }
                break;

            case 'd':

                if ( memcmp(G.f_bssid, NULL_MAC, 6) != 0 )
                {
                    printf("Notice: bssid already given\n");
                    break;
                }
                if(getmac(optarg, 1, G.f_bssid) != 0)
                {
                    printf("Notice: invalid bssid\n");
                    printf("\"%s --help\" for help.\n", argv[0]);

                    return( 1 );
                }
                break;

            case 't':

                set_encryption_filter(optarg);
                break;

            case 'H':

  	            printf( usage, getVersion("Airodump-ng", _MAJ, _MIN, _SUB_MIN, _REVISION, _BETA)  );
  	            return( 1 );

            default : goto usage;
        }
    } while ( 1 );

    if( argc - optind != 1 && G.s_file == NULL)
    {
        if(argc == 1)
        {
usage:
            printf( usage, getVersion("Airodump-ng", _MAJ, _MIN, _SUB_MIN, _REVISION, _BETA)  );
        }
        if( argc - optind == 0)
        {
            printf("No interface specified.\n");
        }
        if(argc > 1)
        {
            printf("\"%s --help\" for help.\n", argv[0]);
        }
        return( 1 );
    }

    if( argc - optind == 1 )
        G.s_iface = argv[argc-1];

    if( ( memcmp(G.f_netmask, NULL_MAC, 6) != 0 ) && ( memcmp(G.f_bssid, NULL_MAC, 6) == 0 ) )
    {
        printf("Notice: specify bssid \"--bssid\" with \"--netmask\"\n");
        printf("\"%s --help\" for help.\n", argv[0]);
        return( 1 );
    }

    if ( ivs_only && !G.record_data ) {
        printf( "Missing dump prefix (-w)\n" );
        printf("\"%s --help\" for help.\n", argv[0]);
        return( 1 );
    }

    if(G.s_iface != NULL)
    {
        /* initialize cards */
        cards = init_cards(G.s_iface, iface, wi);

        if(cards <= 0)
            return( 1 );

        for (i = 0; i < cards; i++) {
            fd_raw[i] = wi_fd(wi[i]);
            if (fd_raw[i] > fdh)
                fdh = fd_raw[i];
        }

        chan_count = getchancount(0);

        /* find the interface index */
        /* start a child to hop between channels */

        if( G.channel[0] == 0 )
        {
            pipe( G.ch_pipe );
            pipe( G.cd_pipe );

            signal( SIGUSR1, sighandler );

            if( ! fork() )
            {
                /* reopen cards.  This way parent & child don't share resources for
                * accessing the card (e.g. file descriptors) which may cause
                * problems.  -sorbo
                */
                for (i = 0; i < cards; i++) {
                    strncpy(ifnam, wi_get_ifname(wi[i]), sizeof(ifnam)-1);
                    ifnam[sizeof(ifnam)-1] = 0;

                    wi_close(wi[i]);
                    wi[i] = wi_open(ifnam);
                    if (!wi[i]) {
                            printf("Can't reopen %s\n", ifnam);
                            exit(1);
                    }
                }

                setuid( getuid() );

                channel_hopper(wi, cards, chan_count);
                exit( 1 );
            }
        }
        else
        {
            for( i=0; i<cards; i++ )
            {
                wi_set_channel(wi[i], G.channel[0]);
                G.channel[i] = G.channel[0];
            }
            G.singlechan = 1;
        }
    }

    setuid( getuid() );

    /* check if there is an input file */
    if( G.s_file != NULL )
    {
        if( ! ( G.f_cap_in = fopen( G.s_file, "rb" ) ) )
        {
            perror( "open failed" );
            return( 1 );
        }

        n = sizeof( struct pcap_file_header );

        if( fread( &G.pfh_in, 1, n, G.f_cap_in ) != (size_t) n )
        {
            perror( "fread(pcap file header) failed" );
            return( 1 );
        }

        if( G.pfh_in.magic != TCPDUMP_MAGIC &&
            G.pfh_in.magic != TCPDUMP_CIGAM )
        {
            fprintf( stderr, "\"%s\" isn't a pcap file (expected "
                             "TCPDUMP_MAGIC).\n", G.s_file );
            return( 1 );
        }

        if( G.pfh_in.magic == TCPDUMP_CIGAM )
            SWAP32(G.pfh_in.linktype);

        if( G.pfh_in.linktype != LINKTYPE_IEEE802_11 &&
            G.pfh_in.linktype != LINKTYPE_PRISM_HEADER )
        {
            fprintf( stderr, "Wrong linktype from pcap file header "
                             "(expected LINKTYPE_IEEE802_11) -\n"
                             "this doesn't look like a regular 802.11 "
                             "capture.\n" );
            return( 1 );
        }
    }

    /* open or create the output files */

    if (G.record_data)
    	if( dump_initialize( G.dump_prefix, ivs_only ) )
    	    return( 1 );

    signal( SIGINT,   sighandler );
    signal( SIGSEGV,  sighandler );
    signal( SIGTERM,  sighandler );
    signal( SIGWINCH, sighandler );

    sighandler( SIGWINCH );

    /* start the GPS tracker */

    if (G.usegpsd)
    {
        pipe( G.gc_pipe );
        signal( SIGUSR2, sighandler );

        if( ! fork() )
        {
            gps_tracker();
            exit( 1 );
        }

        usleep( 50000 );
        waitpid( -1, NULL, WNOHANG );
    }

    fprintf( stderr, "\33[?25l\33[2J\n" );

    start_time = time( NULL );
    tt1        = time( NULL );
    tt2        = time( NULL );
    tt3        = time( NULL );
    gettimeofday( &tv3, NULL );

    G.batt     = getBatteryString();

    G.elapsed_time = (char *) calloc( 1, 4 );
    strncpy(G.elapsed_time, "0 s", 4-1);

    while( 1 )
    {
        if( G.do_exit )
        {
            break;
        }

        if( time( NULL ) - tt1 >= 20 )
        {
            /* update the csv stats file */

            tt1 = time( NULL );
            dump_write_csv();

            /* sort the APs by power */

            dump_sort_power();
        }

        if( time( NULL ) - tt2 > 3 )
        {
            /* update the battery state */
            free(G.batt);
            G.batt = NULL;

            tt2 = time( NULL );
            G.batt = getBatteryString();

            /* update elapsed time */

            free(G.elapsed_time);
            G.elapsed_time=NULL;
            G.elapsed_time = getStringTimeFromSec(
            difftime(tt2, start_time) );


            /* flush the output files */

            if( G.f_cap != NULL ) fflush( G.f_cap );
            if( G.f_ivs != NULL ) fflush( G.f_ivs );
        }

        gettimeofday( &tv1, NULL );

        cycle_time = 1000000 * ( tv1.tv_sec  - tv3.tv_sec  )
                             + ( tv1.tv_usec - tv3.tv_usec );

        if( cycle_time > 500000 )
        {
            gettimeofday( &tv3, NULL );
            update_rx_quality( );
            if(G.s_iface != NULL)
            {
                check_monitor(wi, fd_raw, &fdh, cards);
                if(G.singlechan)
                {
                    check_channel(wi, cards);
                }
            }
        }

        if(G.s_file != NULL)
        {
            /* Read one packet */
            n = sizeof( pkh );

            if( fread( &pkh, n, 1, G.f_cap_in ) != 1 )
            {
                memset(G.message, '\x00', sizeof(G.message));
                snprintf(G.message, sizeof(G.message), "][ Finished reading input file %s.\n", G.s_file);
                G.s_file = NULL;
                continue;
            }

            if( G.pfh_in.magic == TCPDUMP_CIGAM )
                SWAP32( pkh.caplen );

            n = caplen = pkh.caplen;

            memset(buffer, 0, sizeof(buffer));
            h80211 = buffer;

            if( n <= 0 || n > (int) sizeof( buffer ) )
            {
                memset(G.message, '\x00', sizeof(G.message));
                snprintf(G.message, sizeof(G.message), "][ Finished reading input file %s.\n", G.s_file);
                G.s_file = NULL;
                continue;
            }

            if( fread( h80211, n, 1, G.f_cap_in ) != 1 )
            {
                memset(G.message, '\x00', sizeof(G.message));
                snprintf(G.message, sizeof(G.message), "][ Finished reading input file %s.\n", G.s_file);
                G.s_file = NULL;
                continue;
            }

            if( G.pfh_in.linktype == LINKTYPE_PRISM_HEADER )
            {
                if( h80211[7] == 0x40 )
                    n = 64;
                else
                    n = *(int *)( h80211 + 4 );

                if( n < 8 || n >= (int) caplen )
                    continue;

                memcpy( tmpbuf, h80211, caplen );
                caplen -= n;
                memcpy( h80211, tmpbuf + n, caplen );
            }

            read_pkts++;

            if(read_pkts%10 == 0)
                usleep(1);
        }
        else if(G.s_iface != NULL)
        {
            /* capture one packet */

            FD_ZERO( &rfds );
            for(i=0; i<cards; i++)
            {
                FD_SET( fd_raw[i], &rfds );
            }

            tv0.tv_sec  = G.update_s;
            tv0.tv_usec = (G.update_s == 0) ? REFRESH_RATE : 0;

            gettimeofday( &tv1, NULL );

            if( select( fdh + 1, &rfds, NULL, NULL, &tv0 ) < 0 )
            {
                if( errno == EINTR )
                {
                    gettimeofday( &tv2, NULL );

                    time_slept += 1000000 * ( tv2.tv_sec  - tv1.tv_sec  )
                                        + ( tv2.tv_usec - tv1.tv_usec );

                    continue;
                }
                perror( "select failed" );

                /* Restore terminal */
                fprintf( stderr, "\33[?25h" );
                fflush( stdout );

                return( 1 );
            }
        }
        else
            usleep(1);

        gettimeofday( &tv2, NULL );

        time_slept += 1000000 * ( tv2.tv_sec  - tv1.tv_sec  )
                              + ( tv2.tv_usec - tv1.tv_usec );

        if( time_slept > REFRESH_RATE && time_slept > G.update_s * 1000000)
        {
            time_slept = 0;

            update_dataps();

            /* update the window size */

            if( ioctl( 0, TIOCGWINSZ, &ws ) < 0 )
            {
                ws.ws_row = 25;
                ws.ws_col = 80;
            }

            if( ws.ws_col <   1 ) ws.ws_col =   1;
            if( ws.ws_col > 300 ) ws.ws_col = 300;

            /* display the list of access points we have */

            fprintf( stderr, "\33[1;1H" );
            dump_print( ws.ws_row, ws.ws_col, cards );
            fprintf( stderr, "\33[J" );
            fflush( stdout );
            continue;
        }

        if(G.s_file == NULL && G.s_iface != NULL)
        {
            fd_is_set = 0;

            for(i=0; i<cards; i++)
            {
                if( FD_ISSET( fd_raw[i], &rfds ) )
                {

                    memset(buffer, 0, sizeof(buffer));
                    h80211 = buffer;
                    if ((caplen = wi_read(wi[i], h80211, sizeof(buffer), &ri)) == -1) {
                        wi_read_failed++;
                        if(wi_read_failed > 1)
                        {
                            G.do_exit = 1;
                            break;
                        }
                        memset(G.message, '\x00', sizeof(G.message));
                        snprintf(G.message, sizeof(G.message), "][ interface %s down ", wi_get_ifname(wi[i]));

                        //reopen in monitor mode

                        strncpy(ifnam, wi_get_ifname(wi[i]), sizeof(ifnam)-1);
                        ifnam[sizeof(ifnam)-1] = 0;

                        wi_close(wi[i]);
                        wi[i] = wi_open(ifnam);
                        if (!wi[i]) {
                            printf("Can't reopen %s\n", ifnam);

                            /* Restore terminal */
                            fprintf( stderr, "\33[?25h" );
                            fflush( stdout );

                            exit(1);
                        }

                        fd_raw[i] = wi_fd(wi[i]);
                        if (fd_raw[i] > fdh)
                            fdh = fd_raw[i];

                        break;
//                         return 1;
                    }

                    read_pkts++;

                    wi_read_failed = 0;
                    dump_add_packet( h80211, caplen, &ri, i );
                }
            }
        }
        else if (G.s_file != NULL)
        {
            dump_add_packet( h80211, caplen, &ri, i );
        }
    }

    if(G.batt)
        free(G.batt);

    if(G.elapsed_time)
        free(G.elapsed_time);

    if(G.own_channels)
        free(G.own_channels);

    if(G.prefix)
        free(G.prefix);

    if(G.f_cap_name)
        free(G.f_cap_name);

    if(G.keyout)
        free(G.keyout);

    for(i=0; i<cards; i++)
        wi_close(wi[i]);

    if (G.record_data) {
        dump_write_csv();

        if( G.f_txt != NULL ) fclose( G.f_txt );
        if( G.f_gps != NULL ) fclose( G.f_gps );
        if( G.f_cap != NULL ) fclose( G.f_cap );
        if( G.f_ivs != NULL ) fclose( G.f_ivs );
    }

    if( ! G.save_gps )
    {
        snprintf( (char *) buffer, 4096, "%s-%02d.gps", argv[2], G.f_index );
        unlink(  (char *) buffer );
    }

    ap_prv = NULL;
    ap_cur = G.ap_1st;

    while( ap_cur != NULL )
    {
		// Clean content of ap_cur list (first element: G.ap_1st)
        uniqueiv_wipe( ap_cur->uiv_root );

        list_tail_free(&(ap_cur->packets));

		if (G.detect_anomaly)
        	data_wipe(ap_cur->data_root);

        ap_prv = ap_cur;
        ap_cur = ap_cur->next;
    }

    ap_cur = G.ap_1st;

    while( ap_cur != NULL )
    {
		// Freeing AP List
        ap_next = ap_cur->next;

        if( ap_cur != NULL )
            free(ap_cur);

        ap_cur = ap_next;
    }

    st_cur = G.st_1st;
    st_next= NULL;

    while(st_cur != NULL)
    {
        st_next = st_cur->next;
        free(st_cur);
        st_cur = st_next;
    }

    na_cur = G.na_1st;
    na_next= NULL;

    while(na_cur != NULL)
    {
        na_next = na_cur->next;
        free(na_cur);
        na_cur = na_next;
    }

    fprintf( stderr, "\33[?25h" );
    fflush( stdout );

    return( 0 );
}
