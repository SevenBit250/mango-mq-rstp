/* v4l2_fps - 测量 sunxi VIN 的实际帧率 (multi-planar)
 * 用法: v4l2_fps [width] [height] [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <time.h>

struct buf { void *start; size_t len; };

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 640;
    int h = argc > 2 ? atoi(argv[2]) : 480;
    int secs = argc > 3 ? atoi(argv[3]) : 3;
    int nbuf = argc > 4 ? atoi(argv[4]) : 4;
    int fd, i, type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_input in = { .index = 0 };
    struct v4l2_format fmt;
    struct v4l2_requestbuffers rb;
    struct v4l2_buffer bq;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct buf bufs[8];
    unsigned frames = 0;
    long t0, t1, first = 0;

    fd = open("/dev/video0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, VIDIOC_S_INPUT, &in) < 0) { perror("S_INPUT"); return 1; }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix_mp.width = w;
    fmt.fmt.pix_mp.height = h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV21;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
    printf("fmt: %ux%u fourcc=%c%c%c%c\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           fmt.fmt.pix_mp.pixelformat & 0xff,
           (fmt.fmt.pix_mp.pixelformat >> 8) & 0xff,
           (fmt.fmt.pix_mp.pixelformat >> 16) & 0xff,
           (fmt.fmt.pix_mp.pixelformat >> 24) & 0xff);

    memset(&rb, 0, sizeof(rb));
    rb.count = nbuf; rb.type = type; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS"); return 1; }

    for (i = 0; i < nbuf; i++) {
        memset(&bq, 0, sizeof(bq)); memset(planes, 0, sizeof(planes));
        bq.type = type; bq.memory = V4L2_MEMORY_MMAP; bq.index = i; bq.m.planes = planes; bq.length = 1;
        if (ioctl(fd, VIDIOC_QUERYBUF, &bq) < 0) { perror("QUERYBUF"); return 1; }
        bufs[i].len = planes[0].length;
        bufs[i].start = mmap(NULL, bufs[i].len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, planes[0].m.mem_offset);
        if (bufs[i].start == MAP_FAILED) { perror("mmap"); return 1; }
        memset(&bq, 0, sizeof(bq)); memset(planes, 0, sizeof(planes));
        bq.type = type; bq.memory = V4L2_MEMORY_MMAP; bq.index = i; bq.m.planes = planes; bq.length = 1;
        if (ioctl(fd, VIDIOC_QBUF, &bq) < 0) { perror("QBUF"); return 1; }
    }

    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }
    t0 = now_ms();

    while (1) {
        fd_set fds; struct timeval tv;
        FD_ZERO(&fds); FD_SET(fd, &fds);
        tv.tv_sec = 2; tv.tv_usec = 0;
        int r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r <= 0) { printf("timeout after %u frames\n", frames); break; }
        memset(&bq, 0, sizeof(bq)); memset(planes, 0, sizeof(planes));
        bq.type = type; bq.memory = V4L2_MEMORY_MMAP; bq.m.planes = planes; bq.length = 1;
        if (ioctl(fd, VIDIOC_DQBUF, &bq) < 0) { perror("DQBUF"); break; }
        frames++;
        if (frames == 1) { first = now_ms(); }
        if (ioctl(fd, VIDIOC_QBUF, &bq) < 0) { perror("QBUF2"); break; }
        if (now_ms() - t0 >= secs * 1000L) break;
    }

    t1 = now_ms();
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    if (frames > 1 && t1 > first)
        printf("frames=%u  duration=%ldms  fps=%.2f  (first frame %ldms)\n",
               frames, t1 - t0, (frames - 1) * 1000.0 / (double)(t1 - first), first - t0);
    close(fd);
    return 0;
}
