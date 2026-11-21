/* bcm overlay v123: host test for apps/bcm_cab2cav.c (the CABAC -> CAVLC transcoder).
 *
 *   gcc -O2 -DC2C_STATS=1 -Iapps -o cab2cav_test tools/bcm/test/cab2cav_test.c apps/bcm_cab2cav.c
 *   ffmpeg -i in.mp4 -map 0:v -c copy -bsf:v h264_mp4toannexb in.264
 *   ./cab2cav_test in.264 out.264
 *   ffmpeg -i in.264 -f framemd5 a.md5 ; ffmpeg -i out.264 -f framemd5 b.md5    (same picture hashes = bit-exact)
 *
 * in.264 / out.264 are Annex B elementary streams.  The output is CAVLC with the same pictures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bcm_cab2cav.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s in.264 out.264 [escape]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *in = malloc(len + 4);
    if (!in || fread(in, 1, len, f) != (size_t)len) return 1;
    fclose(f);
    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); return 1; }
    void *mem = malloc(c2c_state_size());
    c2c_t *c = c2c_init(mem, c2c_state_size());
    if (argc > 3) c2c_set_escape(c, atoi(argv[3]));
    uint8_t *out = malloc(4 << 20), *tmp = malloc(4 << 20);
    long i = 0; int nal = 0, errors = 0; double t = 0;
    while (i + 3 < len) {
        if (!(in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1)) { i++; continue; }
        long s = i + 3, e = s;
        while (e + 2 < len && !(in[e] == 0 && in[e + 1] == 0 && (in[e + 2] == 1 || in[e + 2] == 0))) e++;
        if (e + 2 >= len) e = len;
        long ee = e; while (ee > s && in[ee - 1] == 0) ee--;
        clock_t t0 = clock();
        int r = c2c_nal(c, in + s, (int)(ee - s), out, 4 << 20, tmp);
        t += (double)(clock() - t0) / CLOCKS_PER_SEC;
        if (r < 0) {
            if (errors++ < 5) fprintf(stderr, "NAL %d (type %d): %s\n", nal, in[s] & 31, c2c_errstr(r));
        } else { fwrite("\0\0\0\1", 1, 4, o); fwrite(out, 1, r, o); }
        nal++; i = e;
    }
    fclose(o);
    const c2c_stats_t *st = c2c_stats(c);
    printf("NAL units %u, slices %u, macroblocks %u (%u skipped), CABAC bins %u, %u -> %u bytes (%+.1f %%), "
           "%u level(s) clipped, %u escaped, %d error(s), %.3f s\n", st->nals, st->slices, st->mbs, st->skipped, st->bins,
           st->in_bytes, st->out_bytes, 100.0 * st->out_bytes / st->in_bytes - 100, st->clamped, st->escaped, errors, t);
    return errors ? 1 : 0;
}
