/* test_queue.c - Sample queue + read()/write() smoke test
 *
 * Build:  gcc -I../include -o test_queue test_queue.c
 * Run:    sudo ./test_queue
 *
 * NOTE: for the seq-numbering check to be meaningful in the strictest
 * sense (driver-assigned, monotonically increasing) this does not require
 * a freshly-reloaded module -- see check_seq_is_driver_assigned() below,
 * which no longer assumes the counter starts at exactly 1.
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include "vmonitor_uapi.h"

#define DEV_PATH "/dev/vmonitor"

static int g_failures = 0;

static void check(int cond, const char *msg)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond)
        g_failures++;
}

static struct vmonitor_sample make_sample(int value_mC, int alarm)
{
    struct vmonitor_sample s;
    memset(&s, 0, sizeof(s));
    s.seq = 0;              /* driver overwrites this */
    s.timestamp_ns = 0;     /* driver overwrites this too (ktime_get_ns()) */
    s.value_mC = value_mC;
    s.alarm = alarm;        /* driver overwrites this based on its own threshold */
    return s;
}

int main(void)
{
    int fd;
    struct vmonitor_sample in, out;
    ssize_t n;
    unsigned long long first_seq;

    fd = open(DEV_PATH, O_RDWR);
    check(fd >= 0, "open basarili");
    if (fd < 0) { perror("open"); return 1; }

    /* 1) Basic write -> read roundtrip */
    in = make_sample(25500, 1 /* driver should ignore this, 25500 < default threshold */);
    n = write(fd, &in, sizeof(in));
    check(n == (ssize_t)sizeof(in), "write() 24 byte yazdi");

    n = read(fd, &out, sizeof(out));
    check(n == (ssize_t)sizeof(out), "read() 24 byte okudu");
    check(out.value_mC == 25500, "value_mC dogru geldi");

    /* Seq numbering: we do NOT assume the module was just freshly loaded
     * (a previous test run, or another program, may have already advanced
     * the counter). What we CAN assert without that assumption:
     *   - it's nonzero, proving the driver assigned it (we sent 0)
     *   - it becomes our baseline for checking monotonic growth below
     * If you specifically want to verify the counter starts at exactly 1,
     * reload the module first: sudo rmmod vmonitor && sudo insmod vmonitor.ko
     */
    first_seq = out.seq;
    check(first_seq != 0, "seq surucu tarafindan atanmis (0 degil, biz 0 gonderdik)");

    check(out.alarm == 0, "25500 < varsayilan esik (35000) icin alarm=0 (surucu hesapladi)");

    n = read(fd, &out, sizeof(out));
    check(n == 0, "bos kuyrukta read() 0 donuyor");

    /* Kesin boyut kontrolu: eksik/fazla byte ile read/write artik -EINVAL vermeli */
    {
        char small_buf[10];
        n = read(fd, small_buf, sizeof(small_buf));
        check(n < 0 && errno == EINVAL, "read() count != 24 icin -EINVAL donuyor");

        n = write(fd, small_buf, sizeof(small_buf));
        check(n < 0 && errno == EINVAL, "write() count != 24 icin -EINVAL donuyor");
    }

    /* 2) Fill the queue completely (64 entries), then overflow it */
    for (int i = 0; i < 64; i++) {
        in = make_sample(10000 + i, 0);
        n = write(fd, &in, sizeof(in));
        if (n != (ssize_t)sizeof(in)) {
            printf("[FAIL] 64 ornek doldurulurken write basarisiz oldu (i=%d)\n", i);
            close(fd);
            return 1;
        }
    }
    printf("[PASS] kuyruk 64 ornekle dolduruldu\n");

    /* 65th sample should be DROPPED (queue full), not overwrite the oldest */
    in = make_sample(99999, 1);
    n = write(fd, &in, sizeof(in));
    check(n == (ssize_t)sizeof(in), "65. write() yine de basarili donuyor (drop, hata degil)");

    /* 3) Full FIFO order verification: drain ALL 64 entries and confirm
     * they come out in the exact order they were pushed (10000..10063),
     * with seq strictly increasing by 1 each time -- not just spot-check
     * the first one. */
    {
        int fifo_ok = 1;
        int seq_ok = 1;
        unsigned long long expected_seq = first_seq + 1; /* next after our roundtrip sample */

        for (int i = 0; i < 64; i++) {
            n = read(fd, &out, sizeof(out));
            if (n != (ssize_t)sizeof(out)) {
                printf("[FAIL] FIFO drain: read basarisiz oldu (i=%d)\n", i);
                fifo_ok = 0;
                break;
            }
            if (out.value_mC != 10000 + i) {
                printf("[FAIL] FIFO drain: beklenen value_mC=%d, gelen=%d (i=%d)\n",
                       10000 + i, out.value_mC, i);
                fifo_ok = 0;
            }
            if (out.seq != expected_seq) {
                printf("[FAIL] FIFO drain: beklenen seq=%llu, gelen=%llu (i=%d)\n",
                       expected_seq, (unsigned long long)out.seq, i);
                seq_ok = 0;
            }
            expected_seq++;
        }
        check(fifo_ok, "64 ornegin tamami dogru FIFO sirasiyla (10000..10063) geldi");
        check(seq_ok, "seq degerleri kesintisiz ardisik artiyor");
    }

    /* Queue must now be empty: the dropped 65th sample never made it in. */
    n = read(fd, &out, sizeof(out));
    check(n == 0, "tam drenajdan sonra kuyruk bos (65. ornek gercekten dusmus)");

    close(fd);
    printf("\nTum testler tamamlandi. Basarisiz kontrol sayisi: %d\n", g_failures);
    return g_failures ? 1 : 0;
}