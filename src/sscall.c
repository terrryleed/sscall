/* See LICENSE file for copyright and license details */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>

#include <ao/ao.h>
#include <pthread.h>
#include <speex/speex.h>
#include <speex/speex_jitter.h>
#include "speex_jitter_buffer.h"

#include "list.h"

/* Input/Output PCM buffer size */
#define FRAME_SIZE (320)
/* Input/Output compressed buffer size */
#define COMPRESSED_BUF_SIZE (1500)

/* Command line option, bits per sample */
static int fbits;
/* Command line option, samples per second (in a single channel) */
static int frate;
/* Command line option, number of channels */
static int fchan;
/* Command line option, device driver ID */
static int fdevid;
/* Command line option, verbosity flag */
static int fverbose;

/* Libspeex encoder state */
static void *speex_enc_state;
/* Libspeex decoder state */
static void *speex_dec_state;
/* Speex Jitter buffer */
static SpeexJitter speex_jitter;
/* Libao handle */
static ao_device *device;
/* Output PCM thread */
static pthread_t playback_thread;
/* Input PCM thread */
static pthread_t capture_thread;

struct compressed_header {
	/* Start of frame signature */
	uint32_t sig;
	uint32_t timestamp;
} __attribute__ ((packed));

/* Shared buf between enqueue_for_playback()
 * and playback thread */
struct compressed_buf {
	/* Compressed buffer */
	char *buf;
	/* Compressed buffer size */
	size_t len;
	struct list_head list;
} compressed_buf;

/* Private structure for the
 * capture thread */
struct capture_priv {
	/* Input file descriptor */
	int fd;
	/* Client socket */
	int sockfd;
	/* Client address info */
	struct addrinfo *servinfo;
} capture_priv;

/* Lock that protects compressed_buf */
static pthread_mutex_t compressed_buf_lock;
/* Condition variable on which ao_play() blocks */
static pthread_cond_t tx_pcm_cond;

/* State of the playback thread */
struct playback_state {
	int quit;
} playback_state;

/* State of the capture thread */
struct capture_state {
	int quit;
} capture_state;

/* Lock that protects playback_state */
static pthread_mutex_t playback_state_lock;
/* Lock that protects capture_state */
static pthread_mutex_t capture_state_lock;
/* Lock that protects the src_state */
static pthread_mutex_t src_state_lock;
/* Lock that protects Speex jitter buffer */
static pthread_mutex_t speex_jitter_lock;

/* Set to 1 when SIGINT is received */
static volatile int handle_sigint;

/* Play back audio from the client */
static void *
playback(void *data)
{
	struct compressed_buf *cbuf;
	struct list_head *iter, *q;
	struct playback_state *state = data;
	struct timespec ts;
	struct timeval tp;
	int rc;
	spx_int16_t pcm[FRAME_SIZE];

	do {
		pthread_mutex_lock(&compressed_buf_lock);
		gettimeofday(&tp, NULL);
		/* Convert from timeval to timespec */
		ts.tv_sec = tp.tv_sec;
		ts.tv_nsec = tp.tv_usec * 1000;
		/* Default to a 3 second wait internal */
		ts.tv_sec += 3;

		if (list_empty(&compressed_buf.list)) {
			/* Wait in the worst case 3 seconds to give some
			 * grace to perform cleanup if necessary */
			rc = pthread_cond_timedwait(&tx_pcm_cond,
						    &compressed_buf_lock,
						    &ts);
			if (rc == ETIMEDOUT)
				if (fverbose)
					printf("Output thread is starving...\n");
		}

		pthread_mutex_lock(&playback_state_lock);
		if (state->quit) {
			pthread_mutex_unlock(&playback_state_lock);
			pthread_mutex_unlock(&compressed_buf_lock);
			break;
		}
		pthread_mutex_unlock(&playback_state_lock);

		/* Dequeue, decode and play buffers via libao */
		list_for_each_safe(iter, q, &compressed_buf.list) {
			cbuf = list_entry(iter, struct compressed_buf,
					  list);

			/* Decode compressed buffer */
			pthread_mutex_lock(&speex_jitter_lock);
			speex_jitter_get(&speex_jitter, pcm, 0);
			pthread_mutex_unlock(&speex_jitter_lock);

			/* Play via libao */
			ao_play(device, (void *)pcm, sizeof(pcm));

			free(cbuf->buf);
			list_del(&cbuf->list);
			free(cbuf);
		}
		pthread_mutex_unlock(&compressed_buf_lock);
	} while (1);

	pthread_exit(NULL);

	return NULL;
}

static void
enqueue_for_playback(struct compressed_buf *cbuf)
{
	pthread_mutex_lock(&compressed_buf_lock);
	INIT_LIST_HEAD(&cbuf->list);
	list_add_tail(&cbuf->list, &compressed_buf.list);
	pthread_cond_signal(&tx_pcm_cond);
	pthread_mutex_unlock(&compressed_buf_lock);
}

/* Parse the compressed packet and enqueue it for
 * playback */
static void
process_compressed_packet(const void *buf, size_t len)
{
	struct compressed_buf *cbuf;
	int recv_timestamp;
	struct compressed_header *hdr;

	cbuf = malloc(sizeof(*cbuf));
	if (!cbuf)
		err(1, "malloc");
	memset(cbuf, 0, sizeof(*cbuf));

	cbuf->len = len - sizeof(*hdr);
	cbuf->buf = malloc(cbuf->len);
	if (!cbuf->buf)
		err(1, "malloc");

	memcpy(cbuf->buf, buf + sizeof(*hdr),
	       cbuf->len);

	hdr = (struct compressed_header *)buf;
	recv_timestamp = hdr->timestamp;

	pthread_mutex_lock(&speex_jitter_lock);
	speex_jitter_put(&speex_jitter, cbuf->buf,
			 cbuf->len, recv_timestamp);
	pthread_mutex_unlock(&speex_jitter_lock);

	enqueue_for_playback(cbuf);
}

/* Input PCM thread, outbound path */
static void *
capture(void *data)
{
	struct capture_state *state = data;
	SpeexBits bits;
	spx_int16_t inbuf[FRAME_SIZE];
	char outbuf[COMPRESSED_BUF_SIZE];
	ssize_t inbytes;
	size_t outbytes;
	ssize_t ret;
	int timestamp;
	struct compressed_header *hdr;

	speex_bits_init(&bits);
	timestamp = 0;
	do {
		pthread_mutex_lock(&capture_state_lock);
		if (state->quit) {
			pthread_mutex_unlock(&capture_state_lock);
			break;
		}
		pthread_mutex_unlock(&capture_state_lock);

		inbytes = read(capture_priv.fd, inbuf, sizeof(inbuf));
		if (inbytes > 0) {
			speex_bits_reset(&bits);
			/* Encode input buffer */
			speex_encode_int(speex_enc_state, inbuf, &bits);
			/* Fill up the buffer with the encoded stream */
			outbytes = speex_bits_write(&bits,
						    outbuf + sizeof(*hdr),
						    sizeof(outbuf) - sizeof(*hdr));
			/* Pre-append the header */
			hdr = (struct compressed_header *)outbuf;
			hdr->sig = htonl(0xcafebabe);
			hdr->timestamp = timestamp;
			timestamp += FRAME_SIZE;
			/* Send the buffer out */
			ret = sendto(capture_priv.sockfd, outbuf,
				     outbytes + sizeof(*hdr), 0,
				     capture_priv.servinfo->ai_addr,
				     capture_priv.servinfo->ai_addrlen);
			if (ret < 0)
				warn("sendto");
		}
	} while (1);

	speex_bits_destroy(&bits);

	pthread_exit(NULL);

	return NULL;
}

static void
usage(const char *s)
{
	fprintf(stderr,
		"usage: %s [OPTIONS] <remote-addr> <remote-port> <local-port>\n", s);
	fprintf(stderr, " -b\tBits per sample\n");
	fprintf(stderr, " -r\tSamples per second (in a single channel)\n");
	fprintf(stderr, " -c\tNumber of channels\n");
	fprintf(stderr, " -d\tOverride default driver ID\n");
	fprintf(stderr, " -v\tEnable verbose output\n");
	fprintf(stderr, " -V\tPrint version information\n");
	fprintf(stderr, " -h\tThis help screen\n");
}

static void
sig_handler(int signum)
{
	switch (signum) {
	case SIGINT:
		handle_sigint = 1;
		break;
	case SIGUSR1:
		fverbose = !fverbose;
		break;
	default:
		break;
	}
}

static void
set_nonblocking(int fd)
{
	int opts;

	opts = fcntl(fd, F_GETFL);
	if (opts < 0)
		err(1, "fcntl");
	opts = (opts | O_NONBLOCK);
	if (fcntl(fd, F_SETFL, opts) < 0)
		err(1, "fcntl");
}

static void
init_ao(int rate, int bits, int chans,
	int *devid)
{
	ao_sample_format format;
	int default_driver;

	ao_initialize();

	default_driver = ao_default_driver_id();

	memset(&format, 0, sizeof(format));
	format.bits = bits;
	format.channels = chans;
	format.rate = rate;
	format.byte_format = AO_FMT_LITTLE;

	if (!*devid)
		*devid = default_driver;

	device = ao_open_live(*devid, &format, NULL);
	if (!device)
		errx(1, "Error opening output device: %d\n",
		     fdevid);
}

static void
init_speex(void)
{
	int tmp;

	/* Create a new encoder/decoder state in wideband mode */
	speex_enc_state = speex_encoder_init(&speex_wb_mode);
	speex_dec_state = speex_decoder_init(&speex_wb_mode);
	/* Set the encoder's quality */
	tmp = 8;
	speex_encoder_ctl(speex_enc_state, SPEEX_SET_QUALITY, &tmp);
	tmp = 2;
	/* Set the encoder's complexity */
	speex_encoder_ctl(speex_enc_state, SPEEX_SET_COMPLEXITY, &tmp);
	/* Init the Speex Jitter Buffer */
	speex_jitter_init(&speex_jitter, speex_dec_state);
}

static void
deinit_ao(void)
{
	ao_close(device);
	ao_shutdown();
}

static void
deinit_speex(void)
{
	speex_encoder_destroy(speex_enc_state);
	speex_decoder_destroy(speex_dec_state);
	speex_jitter_destroy(&speex_jitter);
}

int
main(int argc, char *argv[])
{
	int recfd = STDIN_FILENO;
	ssize_t bytes;
	char buf[COMPRESSED_BUF_SIZE];
	int cli_sockfd, srv_sockfd;
	struct addrinfo cli_hints, *cli_servinfo, *p0, *p1;
	struct addrinfo srv_hints, *srv_servinfo;
	int rv;
	int ret;
	socklen_t addr_len;
	struct sockaddr_storage their_addr;
	char *prog;
	int c;
	char host[NI_MAXHOST];
	int optval;

	prog = *argv;
	while ((c = getopt(argc, argv, "hb:c:r:d:vV")) != -1) {
		switch (c) {
		case 'h':
			usage(prog);
			exit(0);
			break;
		case 'b':
			fbits = strtol(optarg, NULL, 10);
			break;
		case 'c':
			fchan = strtol(optarg, NULL, 10);
			break;
		case 'r':
			frate = strtol(optarg, NULL, 10);
			break;
		case 'd':
			fdevid = strtol(optarg, NULL, 10);
			break;
		case 'v':
			fverbose = 1;
			break;
		case 'V':
			printf("%s\n", VERSION);
			exit(0);
		case '?':
		default:
			exit(1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 3) {
		usage(prog);
		exit(1);
	}

	if (!fbits)
		fbits = 16;

	if (!fchan)
		fchan = 1;
	else if (fchan != 1)
		errx(1, "Unsupported number of channels: %d",
		     fchan);

	if (!frate)
		frate = 16000;

	init_ao(frate, fbits, fchan, &fdevid);
	init_speex();

	if (fverbose) {
		printf("Bits per sample: %d\n", fbits);
		printf("Number of channels: %d\n", fchan);
		printf("Sample rate: %d\n", frate);
		printf("Default driver ID: %d\n", fdevid);
		fflush(stdout);
	}

	memset(&cli_hints, 0, sizeof(cli_hints));
	cli_hints.ai_family = AF_INET;
	cli_hints.ai_socktype = SOCK_DGRAM;

	rv = getaddrinfo(argv[0], argv[1], &cli_hints, &cli_servinfo);
	if (rv)
		errx(1, "getaddrinfo: %s", gai_strerror(rv));

	for (p0 = cli_servinfo; p0; p0 = p0->ai_next) {
		cli_sockfd = socket(p0->ai_family, p0->ai_socktype,
				    p0->ai_protocol);
		if (cli_sockfd < 0)
			continue;
		break;
	}

	if (!p0)
		errx(1, "failed to bind socket");

	memset(&srv_hints, 0, sizeof(srv_hints));
	srv_hints.ai_family = AF_INET;
	srv_hints.ai_socktype = SOCK_DGRAM;
	srv_hints.ai_flags = AI_PASSIVE;

	rv = getaddrinfo(NULL, argv[2], &srv_hints, &srv_servinfo);
	if (rv)
		errx(1, "getaddrinfo: %s", gai_strerror(rv));

	for(p1 = srv_servinfo; p1; p1 = p1->ai_next) {
		srv_sockfd = socket(p1->ai_family, p1->ai_socktype,
				    p1->ai_protocol);
		if (srv_sockfd < 0)
			continue;
		optval = 1;
		ret = setsockopt(srv_sockfd, SOL_SOCKET,
				 SO_REUSEADDR, &optval, sizeof(optval));
		if (ret < 0) {
			close(srv_sockfd);
			warn("setsockopt");
			continue;
		}
		if (bind(srv_sockfd, p1->ai_addr, p1->ai_addrlen) < 0) {
			close(srv_sockfd);
			warn("bind");
			continue;
		}
		break;
	}

	if (!p1)
		errx(1, "failed to bind socket");

	INIT_LIST_HEAD(&compressed_buf.list);

	pthread_mutex_init(&compressed_buf_lock, NULL);
	pthread_cond_init(&tx_pcm_cond, NULL);

	pthread_mutex_init(&playback_state_lock, NULL);
	pthread_mutex_init(&capture_state_lock, NULL);
	pthread_mutex_init(&speex_jitter_lock, NULL);

	pthread_mutex_init(&src_state_lock, NULL);

	ret = pthread_create(&playback_thread, NULL,
			     playback, &playback_state);
	if (ret) {
		errno = ret;
		err(1, "pthread_create");
	}

	capture_priv.fd = recfd;
	capture_priv.sockfd = cli_sockfd;
	capture_priv.servinfo = p0;

	set_nonblocking(capture_priv.fd);
	set_nonblocking(capture_priv.sockfd);

	ret = pthread_create(&capture_thread, NULL,
			     capture, &capture_state);
	if (ret) {
		errno = ret;
		err(1, "pthread_create");
	}

	if (signal(SIGINT, sig_handler) == SIG_ERR)
		err(1, "signal");

	if (signal(SIGUSR1, sig_handler) == SIG_ERR)
		err(1, "signal");

	/* Main processing loop, receive compressed data,
	 * parse and prepare for playback */
	do {
		/* Handle SIGINT gracefully */
		if (handle_sigint) {
			if (fverbose)
				printf("Interrupted, exiting...\n");
			break;
		}

		addr_len = sizeof(their_addr);
		bytes = recvfrom(srv_sockfd, buf,
				 sizeof(buf), MSG_DONTWAIT,
				 (struct sockaddr *)&their_addr,
				 &addr_len);
		if (bytes > 0) {
			if (fverbose) {
				ret = getnameinfo((struct sockaddr *)&their_addr,
						  addr_len, host,
						  sizeof(host), NULL, 0, 0);
				if (ret < 0) {
					warn("getnameinfo");
					snprintf(host, sizeof(host), "unknown");
				}
				printf("Received %zd bytes from %s\n",
				       bytes, host);
			}
			process_compressed_packet(buf, bytes);
		}
	} while (1);

	/* Prepare input thread to be killed */
	pthread_mutex_lock(&capture_state_lock);
	capture_state.quit = 1;
	pthread_mutex_unlock(&capture_state_lock);

	/* Wait for it */
	pthread_join(capture_thread, NULL);

	/* Prepare output thread to be killed */
	pthread_mutex_lock(&playback_state_lock);
	playback_state.quit = 1;
	pthread_mutex_unlock(&playback_state_lock);

	/* Wake up the output thread if it is
	 * sleeping */
	pthread_mutex_lock(&compressed_buf_lock);
	pthread_cond_signal(&tx_pcm_cond);
	pthread_mutex_unlock(&compressed_buf_lock);

	/* Wait for it */
	pthread_join(playback_thread, NULL);

	deinit_speex();
	deinit_ao();

	freeaddrinfo(cli_servinfo);
	freeaddrinfo(srv_servinfo);

	return EXIT_SUCCESS;
}
