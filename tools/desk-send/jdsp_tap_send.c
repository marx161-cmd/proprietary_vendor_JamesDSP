// jdsp-tap-send: ship the JamesDSP output tap (tap.bin, written by the full
// variant's tap_publish() in jdspController.c) to the desk speaker receiver
// over UDP as raw s16le interleaved stereo at the engine's sample rate --
// the same wire format comrade's oppo-usb-audio sender uses, so the Oppo's
// `listen-pixel` is a plain ffplay twin of `listen-amd` on another port.
//
// Runs in Termux on the Pixel (UID 1000 = system, which may read the JDSP
// app's files dir). Polls the ring every 5 ms and forwards new frames in
// 240-frame (960-byte) packets; when the effect isn't producing (nothing
// playing, or the tap is disabled in the JDSP app) it sends nothing --
// ffplay plays silence on underrun and resumes on the next packet, so no
// silence filling is needed and latency stays at one HAL block + the network.
//
//   jdsp-tap-send <host:port> [tap.bin path]
//
// Build (Termux): clang -O2 -Wall -o ~/bin/jdsp-tap-send jdsp_tap_send.c
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TAP_MAGIC 0x5054444Au /* "JDTP" */
#define PKT_FRAMES 240u       /* 5 ms at 48 kHz, 960 bytes: under one MTU */
#define POLL_US 5000
#define IDLE_US 100000

typedef struct {
	uint32_t magic, version, seq, ringFrames, channels, sampleRate, flags, reserved;
} TapHeader;

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Map the file; returns NULL if it isn't a v1 tap file yet (the HAL creates
// it at effect init, so this just retries until then).
static TapHeader *map_tap(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return NULL;
	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(TapHeader)) { close(fd); return NULL; }
	void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED) return NULL;
	TapHeader *h = (TapHeader *)p;
	if (h->magic != TAP_MAGIC || h->version != 1 || h->channels != 2 ||
	    h->ringFrames == 0 || (h->ringFrames & (h->ringFrames - 1)) ||
	    (size_t)st.st_size < sizeof(TapHeader) + (size_t)h->ringFrames * 4) {
		munmap(p, (size_t)st.st_size);
		return NULL;
	}
	*len = (size_t)st.st_size;
	return h;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s <host:port> [tap.bin]\n", argv[0]); return 2; }
	const char *path = argc > 2 ? argv[2] : "/data/data/com.termux.jdsp/files/tap.bin";
	char host[256]; const char *colon = strrchr(argv[1], ':');
	if (!colon) { fprintf(stderr, "target must be host:port\n"); return 2; }
	snprintf(host, sizeof host, "%.*s", (int)(colon - argv[1]), argv[1]);
	struct addrinfo hints = {0}, *ai;
	hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_DGRAM;
	int rc = getaddrinfo(host, colon + 1, &hints, &ai);
	if (rc) { fprintf(stderr, "resolve %s: %s\n", argv[1], gai_strerror(rc)); return 2; }
	int sock = socket(ai->ai_family, SOCK_DGRAM, 0);
	if (sock < 0 || connect(sock, ai->ai_addr, ai->ai_addrlen) != 0) { perror("socket"); return 2; }
	freeaddrinfo(ai);
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

	TapHeader *h = NULL; size_t len = 0;
	const int16_t *ring = NULL;
	uint32_t rpos = 0, ringFrames = 0;
	int was_enabled = -1;
	unsigned long sent_pkts = 0, dropped = 0;
	double last_log = now_s();
	int16_t pkt[PKT_FRAMES * 2];

	while (!g_stop) {
		if (!h) {
			h = map_tap(path, &len);
			if (!h) { usleep(IDLE_US * 5); continue; }
			ring = (const int16_t *)((const char *)h + sizeof(TapHeader));
			ringFrames = h->ringFrames;
			rpos = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
			fprintf(stderr, "mapped %s: ring %u frames, %u Hz\n", path, ringFrames, h->sampleRate);
		}
		uint32_t seq = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
		int enabled = (h->flags & 1u) != 0;
		if (enabled != was_enabled) {
			fprintf(stderr, "tap %s (mute local %s)\n", enabled ? "ENABLED" : "disabled",
			        (h->flags & 2u) ? "on" : "off");
			was_enabled = enabled;
			rpos = seq; // never replay stale ring content on enable
		}
		if (!enabled) { usleep(IDLE_US); continue; }
		// A HAL restart memsets the file (seq jumps back); the magic may also
		// vanish for a moment -> remap.
		if (h->magic != TAP_MAGIC) { munmap(h, len); h = NULL; continue; }
		if (seq < rpos) rpos = seq;
		uint32_t avail = seq - rpos;
		if (avail > ringFrames) { // fell behind: skip to the newest quarter ring
			dropped += avail - ringFrames / 4;
			rpos = seq - ringFrames / 4;
			avail = ringFrames / 4;
		}
		while (avail >= PKT_FRAMES) {
			for (unsigned i = 0; i < PKT_FRAMES; i++) {
				uint32_t slot = (rpos + i) & (ringFrames - 1);
				pkt[2 * i] = ring[2 * slot];
				pkt[2 * i + 1] = ring[2 * slot + 1];
			}
			// Torn read guard: if the writer lapped us during the copy, resync.
			uint32_t seq2 = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
			if (seq2 - rpos > ringFrames) { rpos = seq2 - ringFrames / 4; avail = 0; dropped++; break; }
			if (send(sock, pkt, sizeof pkt, 0) < 0 && errno != ECONNREFUSED && errno != ENETUNREACH)
				perror("send");
			sent_pkts++;
			rpos += PKT_FRAMES;
			avail -= PKT_FRAMES;
		}
		double t = now_s();
		if (t - last_log > 60.0) {
			fprintf(stderr, "sent %lu pkts, dropped %lu frames\n", sent_pkts, dropped);
			last_log = t;
		}
		usleep(POLL_US);
	}
	fprintf(stderr, "stopping: sent %lu pkts, dropped %lu frames\n", sent_pkts, dropped);
	return 0;
}
