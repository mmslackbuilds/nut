#include <inttypes.h>
#include <stdlib.h>
#include "nut.h"
#include "priv.h"

static int get_dts(stream_context_t * s, int pts) {
	int d = s->decode_delay;

	while (d--) {
		int64_t t = s->pts_cache[d];
		if (t < pts) {
			s->pts_cache[d] = pts;
			pts = t;
		}
	}
	return pts;
}

static int convert_pts(nut_context_t * nut, int from, int to, int pts) {
	int64_t ln, timestamp = pts, d1, d2;

	d1 = nut->sc[from].sh.time_base_denom;
	ln = (int64_t)nut->sc[from].sh.time_base_nom * nut->sc[to].sh.time_base_denom;
	d2 = nut->sc[to].sh.time_base_nom;
	return (ln / d1 * timestamp + (ln % d1) * timestamp / d1) / d2;
}

static void shift_frames(nut_context_t * nut, stream_context_t * s, int amount) {
	int i;
	assert(amount <= s->num_packets);
	for (i = 0; i < amount; i++) {
		nut_write_frame(nut, &s->packets[i].p, s->packets[i].buf);
		free(s->packets[i].buf); // FIXME
	}
	s->next_pts = s->packets[i - 1].p.next_pts;
	s->num_packets -= amount;

	memmove(s->packets, s->packets + amount, s->num_packets * sizeof(reorder_packet_t));
	s->packets = realloc(s->packets, s->num_packets * sizeof(reorder_packet_t));
}

static void flushcheck_frames(nut_context_t * nut) {
	int change, i;
	for (i = 0; i < nut->stream_count; i++) {
		// check if any streams are missing essential info
		if (!nut->sc[i].num_packets && !nut->sc[i].next_pts) return;
	}
	do {
		change = 0;
		for (i = 0; i < nut->stream_count; i++) {
			int j, min = -1;
			if (!nut->sc[i].num_packets) continue; // no packets pending in this stream
			for (j = 0; j < nut->stream_count; j++) {
				int pts;
				if (i == j) continue;

				if (nut->sc[j].num_packets) pts = nut->sc[j].packets[0].p.pts;
				else pts = nut->sc[j].next_pts;

				if (pts != -1) {
					pts = convert_pts(nut, j, i, pts);
					min = MIN(min, pts);
					if (min == -1) min = pts;
				}
			}
			// MN rule, (i < j) && (i.dts <= j.pts)
			if (min == -1 || nut->sc[i].packets[0].dts <= min) {
				for (j = 1; j < nut->sc[i].num_packets; j++) {
					if (min != -1 && nut->sc[i].packets[j].dts > min) break;
				}
				shift_frames(nut, &nut->sc[i], j);
				change = 1;
			}
		}
	} while (change);
}

void nut_muxer_uninit_reorder(nut_context_t * nut) {
	int i;
	if (!nut) return;

	for (i = 0; i < nut->stream_count; i++) nut->sc[i].next_pts = -1;
	flushcheck_frames(nut);

	for (i = 0; i < nut->stream_count; i++) {
		assert(!nut->sc[i].num_packets);
		free(nut->sc[i].packets);
		nut->sc[i].packets = NULL;
	}
	nut_muxer_uninit(nut);
}

void nut_write_frame_reorder(nut_context_t * nut, const nut_packet_t * p, const uint8_t * buf) {
	stream_context_t * s = &nut->sc[p->stream];
	if (nut->stream_count < 2) { // do nothing
		nut_write_frame(nut, p, buf);
		return;
	}

	s->num_packets++;
	s->packets = realloc(s->packets, s->num_packets * sizeof(reorder_packet_t));
	s->packets[s->num_packets - 1].p = *p;
	s->packets[s->num_packets - 1].dts = get_dts(s, p->pts);

	s->packets[s->num_packets - 1].buf = malloc(p->len); // FIXME
	memcpy(s->packets[s->num_packets - 1].buf, buf, p->len);

	flushcheck_frames(nut);
}
