/* capdump - 用 mmap 方式抓一帧 NV21 存盘, 用于对比 USERPTR 结果 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <linux/videodev2.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

static void i2c_wr(unsigned reg, unsigned val)
{
    int fd = open("/dev/i2c-2", O_RDWR);
    unsigned char b[3];
    if (fd < 0) { perror("i2c open"); return; }
    ioctl(fd, I2C_SLAVE_FORCE, 0x3c);
    b[0] = reg >> 8; b[1] = reg & 0xff; b[2] = val & 0xff;
    if (write(fd, b, 3) != 3) perror("i2c write");
    else printf("i2c 0x%04x = 0x%02x\n", reg, val);
    close(fd);
}

int main(int argc, char **argv)
{
    int fd = open("/dev/video0", O_RDWR);
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_input in = { .index = 0 };
    struct v4l2_format fmt;
    struct v4l2_requestbuffers rb;
    struct v4l2_buffer bq;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    void *p[4];
    size_t len[4];
    int i, first_i = 0;
    struct timespec first_t = {0, 0};

    if (fd < 0) { perror("open"); return 1; }
    ioctl(fd, VIDIOC_S_INPUT, &in);
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix_mp.width = argc > 1 ? atoi(argv[1]) : 1280;
    fmt.fmt.pix_mp.height = argc > 2 ? atoi(argv[2]) : 720;
    if (argc > 3 && argv[3][0] == 'G')
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_GREY;
    else if (argc > 3 && argv[3][0] == 'Y')
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUYV;
    else
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV21;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
    printf("fmt now %ux%u planes=%u\n", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           fmt.fmt.pix_mp.num_planes);

    /* argc>5 时在流运行中第 10 帧后写寄存器, 避免被驱动的模式表覆盖 */

    memset(&rb, 0, sizeof(rb));
    rb.count = 4;
    rb.type = type;
    rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS"); return 1; }

    for (i = 0; i < (int)rb.count; i++) {
        memset(&bq, 0, sizeof(bq));
        memset(planes, 0, sizeof(planes));
        bq.type = type; bq.memory = V4L2_MEMORY_MMAP; bq.index = i;
        bq.m.planes = planes; bq.length = 1;
        if (ioctl(fd, VIDIOC_QUERYBUF, &bq) < 0) { perror("QUERYBUF"); return 1; }
        len[i] = planes[0].length;
        p[i] = mmap(NULL, len[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                    planes[0].m.mem_offset);
        if (p[i] == MAP_FAILED) { perror("mmap"); return 1; }
        if (ioctl(fd, VIDIOC_QBUF, &bq) < 0) { perror("QBUF"); return 1; }
    }
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }

    for (i = 0; i < 20; i++) {
        fd_set fds; struct timeval tv = { 2, 0 };
        FD_ZERO(&fds); FD_SET(fd, &fds);
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) { printf("timeout\n"); break; }
        memset(&bq, 0, sizeof(bq)); memset(planes, 0, sizeof(planes));
        bq.type = type; bq.memory = V4L2_MEMORY_MMAP; bq.m.planes = planes; bq.length = 1;
        if (ioctl(fd, VIDIOC_DQBUF, &bq) < 0) { perror("DQBUF"); break; }
        if (argc > 5 && i == 10) {
            int a;
            for (a = 4; a + 1 < argc; a += 2)
                i2c_wr((unsigned)strtoul(argv[a], NULL, 16),
                       (unsigned)strtoul(argv[a + 1], NULL, 16));
            usleep(500000);
            first_i = i;
            clock_gettime(CLOCK_MONOTONIC, &first_t);
        }
        if (i == 18) {
            struct timespec now_t;
            clock_gettime(CLOCK_MONOTONIC, &now_t);
            if (first_i > 0) {
                double dt = (now_t.tv_sec - first_t.tv_sec) +
                            (now_t.tv_nsec - first_t.tv_nsec) / 1e9;
                printf("fps after change: %.2f (%d frames in %.3fs)\n",
                       (i - first_i) / dt, i - first_i, dt);
            }
            int f = open("/tmp/capdump.yuv", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            {
                size_t need = (size_t)fmt.fmt.pix_mp.width * fmt.fmt.pix_mp.height;
                if (fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YUYV)
                    need *= 2;
                else if (fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV21)
                    need = need * 3 / 2;
                write(f, p[bq.index], planes[0].bytesused ? planes[0].bytesused : need);
            }
            close(f);
            printf("saved frame %d idx=%d len=%zu\n", i, bq.index, len[bq.index]);
        }
        ioctl(fd, VIDIOC_QBUF, &bq);
    }
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    close(fd);
    return 0;
}
