/* test_queue.c - Sample queue + read()/write() smoke test
 *
 * Build:  gcc -I../include -o test_queue test_queue.c
 * Run:    sudo ./test_queue
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include "vmonitor_uapi.h"

#define DEV_PATH "/dev/vmonitor"

static void check(int cond, const char *msg)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
}

static struct vmonitor_sample make_sample(int value_mC, int alarm)
{
    struct vmonitor_sample s;
    memset(&s, 0, sizeof(s));
    s.seq = 0;              /* driver overwrites this */
    s.timestamp_ns = 0;     /* not used yet in this task */
    s.value_mC = value_mC;
    s.alarm = alarm;
    return s;
}

int main(void)
{
    int fd;
    struct vmonitor_sample in, out;
    ssize_t n;

    fd = open(DEV_PATH, O_RDWR);
    check(fd >= 0, "open basarili");
    if (fd < 0) { perror("open"); return 1; }

    /* 1) Basic write -> read roundtrip */
    in = make_sample(25500, 0);
    n = write(fd, &in, sizeof(in));
    check(n == (ssize_t)sizeof(in), "write() 24 byte yazdi");

    n = read(fd, &out, sizeof(out));
    check(n == (ssize_t)sizeof(out), "read() 24 byte okudu");
    check(out.value_mC == 25500, "value_mC dogru geldi");
    check(out.seq == 1, "ilk seq == 1 (surucu atadi)");

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

    /* The oldest unread sample must still be the first one we wrote (value 10000) */
    n = read(fd, &out, sizeof(out));
    check(n == (ssize_t)sizeof(out), "dolu kuyruktan read basarili");
    check(out.value_mC == 10000, "en eski (ilk) ornek hala kuyrukta, ezilmemis");
    check(out.value_mC != 99999, "65. (fazla) ornek kuyruga girmedi");

    close(fd);
    printf("\nTum testler tamamlandi.\n");
    return 0;
}