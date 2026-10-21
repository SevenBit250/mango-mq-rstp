/* mq_rtsp - MangoPi Nezha-MQ (D1s) OV5640 720p60 MJPEG RTSP 服务器
 *
 * 采集: /dev/video0 (sunxi VIN, NV21M 双平面, DMABUF/USERPTR)
 * 编码: Allwinner VE 硬件 JPEG (libvencoder, ~4ms/帧@720p)
 * 推流: RTSP/RTP JPEG (RFC 2435, 偏移用字节数以兼容 ffmpeg)
 *
 * 用法: mq_rtsp [kbps] [quality] [port] [width] [height]
 *   例: mq_rtsp 10000 90 8554 1280 720
 *
 * 关键设计:
 *   1. 零拷贝: VIN 直接 DMA 到 vencoder 的 ion 输入缓冲 (USERPTR 双平面),
 *      V4L2 的 mmap 缓冲是 uncached 的, 之前 memcpy 1.4MB 要 40ms.
 *   2. 采集/编码与 RTP 发送分开: 8189fs 每个 UDP 包约 1ms, 不能拖慢采集.
 *   3. 内存: 3 个编码输入帧 4.2MB + 编码器内部 ~3MB + 帧槽 0.5MB
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#include "vencoder.h"
#include "memoryAdapter.h"

#define ALIGN16(x) (((x) + 15) & ~15)
#define MAX_FRAME  (256 * 1024)
#define NSLOT 3
#define RTP_MAX_PAYLOAD 1400
#define THREAD_STACK (128 * 1024)

static int g_w = 1280, g_h = 720;
static int g_bitrate_kbps = 10000;
static int g_quality = 90;
static int g_port = 8554;
static int g_fps = 60;
static int g_nslot = NSLOT;

/* ---- 编码输入缓冲: VIN 直接写入 ---- */
static VencInputBuffer g_vin[NSLOT];

/* ---- 帧槽: 采集线程 -> RTP 线程 ---- */
static pthread_mutex_t g_flock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fcond = PTHREAD_COND_INITIALIZER;
static unsigned char *g_js;            /* 最新 JPEG */
static size_t g_jlen;
static unsigned g_jseq;
static volatile int g_running = 1;

/* ---- 会话 ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    struct sockaddr_in addr;
    int port;
    int playing;
} g_sess;

static unsigned char *g_rtp_buf;
static int g_rtp_fd = -1;
static int g_server_port;
static uint16_t g_seq;
static uint32_t g_ts;
static uint32_t g_ssrc = 0x4d515031;
static char g_base_url[256] = "rtsp://0.0.0.0:8554/stream";

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

static void die(const char *s)
{
    perror(s);
    exit(1);
}

static void mem_stat(const char *tag)
{
    char b[2048];
    int f = open("/proc/meminfo", O_RDONLY);
    int n;

    if (f < 0)
        return;
    n = read(f, b, sizeof(b) - 1);
    close(f);
    if (n <= 0)
        return;
    b[n] = 0;
    {
        char *a = strstr(b, "MemAvailable:");
        char *c = strstr(b, "CmaFree:");
        printf("%s: MemAvailable=%ld kB CmaFree=%ld kB\n", tag,
               a ? atol(a + 14) : -1, c ? atol(c + 9) : -1);
    }
}

/* ---------------- 编码器 ---------------- */

static VideoEncoder *venc_open(void)
{
    VideoEncoder *enc = VideoEncCreate(VENC_CODEC_JPEG);
    if (!enc) {
        fprintf(stderr, "VideoEncCreate(JPEG) failed\n");
        return NULL;
    }

    int vbv = 2 * 1024 * 1024;
    int quality = g_quality;
    int mode = 1;                    /* 1 = 码率模式 */
    int bitrate = g_bitrate_kbps * 1024;
    int framerate = g_fps;
    VencBitRateRange range;
    VencJpegVideoSignal vs;

    range.bitRateMax = bitrate * 5 / 4;
    range.bitRateMin = bitrate * 3 / 4;
    vs.src_colour_primaries = VENC_YCC;
    vs.dst_colour_primaries = VENC_BT601;

    VideoEncSetParameter(enc, VENC_IndexParamSetVbvSize, &vbv);
    VideoEncSetParameter(enc, VENC_IndexParamJpegQuality, &quality);
    VideoEncSetParameter(enc, VENC_IndexParamJpegEncMode, &mode);
    VideoEncSetParameter(enc, VENC_IndexParamJpegVideoSignal, &vs);
    VideoEncSetParameter(enc, VENC_IndexParamBitrate, &bitrate);
    VideoEncSetParameter(enc, VENC_IndexParamFramerate, &framerate);
    VideoEncSetParameter(enc, VENC_IndexParamSetBitRateRange, &range);

    VencBaseConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.memops = MemAdapterGetOpsS();
    if (!cfg.memops) {
        fprintf(stderr, "MemAdapterGetOpsS failed\n");
        goto fail;
    }
    CdcMemOpen(cfg.memops);
    cfg.nInputWidth = g_w;
    cfg.nInputHeight = g_h;
    cfg.nStride = ALIGN16(g_w);
    cfg.nDstWidth = g_w;
    cfg.nDstHeight = g_h;
    cfg.eInputFormat = VENC_PIXEL_YVU420SP;   /* NV21 */

    if (VideoEncInit(enc, &cfg) != 0) {
        fprintf(stderr, "VideoEncInit failed\n");
        goto fail_mem;
    }

    VencAllocateBufferParam bp;
    memset(&bp, 0, sizeof(bp));
    bp.nBufferNum = NSLOT;
    bp.nSizeY = ALIGN16(g_w) * ALIGN16(g_h) + 1024;
    bp.nSizeC = ALIGN16(g_w) * ALIGN16(g_h) / 2 + 1024;
    AllocInputBuffer(enc, &bp);

    /* 全部取出, 交给 VIN 长期持有, 不再归还 */
    for (int i = 0; i < NSLOT; i++) {
        if (GetOneAllocInputBuffer(enc, &g_vin[i]) != 0) {
            fprintf(stderr, "GetOneAllocInputBuffer(%d) failed\n", i);
            goto fail_mem;
        }
    }
    printf("venc: JPEG %dx%d, %dkbps, quality %d, %d zero-copy buffers\n",
           g_w, g_h, g_bitrate_kbps, g_quality, NSLOT);
    return enc;

fail_mem:
    CdcMemClose(cfg.memops);
fail:
    VideoEncDestroy(enc);
    return NULL;
}

/* ---------------- JPEG 拆解 ---------------- */

static int jpeg_split(const unsigned char *j, size_t n, size_t *scan_off,
                      unsigned char qt[2][64])
{
    size_t p = 2;
    int nt = 0;

    if (n < 4 || j[0] != 0xFF || j[1] != 0xD8)
        return -1;

    while (p + 4 <= n) {
        int m;
        size_t len, q, e;

        if (j[p] != 0xFF)
            return -1;
        m = j[p + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            p += 2;
            continue;
        }
        if (m == 0xD9)
            return -1;
        len = ((size_t)j[p + 2] << 8) | j[p + 3];
        if (len < 2 || p + 2 + len > n)
            return -1;

        if (m == 0xDB) {
            q = p + 4;
            e = p + 2 + len;
            while (q < e) {
                int pq = j[q] >> 4, tq = j[q] & 15;
                q++;
                if (tq > 1 || pq != 0 || q + 64 > e)
                    return -1;
                memcpy(qt[tq], j + q, 64);
                nt++;
                q += 64;
            }
        } else if (m == 0xDA) {
            if (nt < 2)
                return -1;
            *scan_off = p + 2 + len;
            return 0;
        }
        p += 2 + len;
    }
    return -1;
}

static void rtp_send_frame(const unsigned char *jpg, size_t len,
                           const struct sockaddr_in *dst)
{
    unsigned char qt[2][64];
    size_t scan, plen, pos = 0;
    int first = 1;

    if (jpeg_split(jpg, len, &scan, qt) != 0)
        return;
    if (len < scan + 2 || jpg[len - 2] != 0xFF || jpg[len - 1] != 0xD9)
        return;
    plen = len - 2 - scan;

    while (pos < plen) {
        unsigned char pkt[RTP_MAX_PAYLOAD];
        int hdr = 12 + 8 + (first ? 4 + 128 : 0);
        size_t chunk = RTP_MAX_PAYLOAD - hdr;
        int marker;

        if (chunk > plen - pos)
            chunk = plen - pos;
        marker = (pos + chunk == plen);

        pkt[0] = 0x80;
        pkt[1] = 26 | (marker ? 0x80 : 0);
        pkt[2] = g_seq >> 8;
        pkt[3] = g_seq & 0xff;
        pkt[4] = g_ts >> 24;
        pkt[5] = g_ts >> 16;
        pkt[6] = g_ts >> 8;
        pkt[7] = g_ts & 0xff;
        pkt[8] = g_ssrc >> 24;
        pkt[9] = g_ssrc >> 16;
        pkt[10] = g_ssrc >> 8;
        pkt[11] = g_ssrc & 0xff;

        /* RFC 2435: ffmpeg 实现的偏移是字节数, 不是 8 字节单位 */
        pkt[12] = 0;
        pkt[13] = pos >> 16;
        pkt[14] = pos >> 8;
        pkt[15] = pos & 0xff;
        pkt[16] = 1;            /* type: 1 = 4:2:0 (ffmpeg 约定) */
        pkt[17] = 255;          /* Q: 量化表随包携带 */
        pkt[18] = g_w >> 3;
        pkt[19] = g_h >> 3;
        if (first) {
            pkt[20] = 0;
            pkt[21] = 0;
            pkt[22] = 0;
            pkt[23] = 128;
            memcpy(pkt + 24, qt[0], 64);
            memcpy(pkt + 88, qt[1], 64);
        }
        memcpy(pkt + hdr, jpg + scan + pos, chunk);

        if (sendto(g_rtp_fd, pkt, hdr + chunk, 0,
                   (const struct sockaddr *)dst, sizeof(*dst)) < 0) {
            if (errno != EAGAIN && errno != ENOBUFS)
                perror("sendto");
        }
        pos += chunk;
        first = 0;
        g_seq++;
    }
    g_ts += 90000 / g_fps;
}

/* ---------------- RTP 发送线程 ---------------- */

static void *rtp_thread(void *arg)
{
    unsigned last = 0;
    unsigned sent = 0;
    uint64_t t0 = now_us(), t1;

    (void)arg;
    while (g_running) {
        struct timespec ts;
        size_t n = 0;
        int playing, cport;
        struct sockaddr_in dst;

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

        pthread_mutex_lock(&g_flock);
        while (g_jseq == last && g_running) {
            if (pthread_cond_timedwait(&g_fcond, &g_flock, &ts) == ETIMEDOUT)
                break;
        }
        if (g_jseq != last) {
            last = g_jseq;
            n = g_jlen;
            if (n > MAX_FRAME)
                n = MAX_FRAME;
            memcpy(g_rtp_buf, g_js, n);
        }
        pthread_mutex_unlock(&g_flock);

        pthread_mutex_lock(&g_lock);
        playing = g_sess.playing && g_sess.port > 0;
        dst = g_sess.addr;
        cport = g_sess.port;
        pthread_mutex_unlock(&g_lock);

        if (n && playing && cport > 0) {
            rtp_send_frame(g_rtp_buf, n, &dst);
            sent++;
        }

        t1 = now_us();
        if (t1 - t0 >= 5000000ull) {
            printf("rtp: %.1f frames/s sent, %u total, last %zu B\n",
                   sent / ((t1 - t0) / 1e6), sent, n);
            sent = 0;
            t0 = t1;
        }
    }
    return NULL;
}

/* ---------------- 采集 + 零拷贝编码 ---------------- */

static void *capture_thread(void *arg)
{
    VideoEncoder *enc = arg;
    int fd = open("/dev/video0", O_RDWR);
    struct v4l2_input in = { .index = 0 };
    struct v4l2_format fmt;
    struct v4l2_requestbuffers rb;
    struct v4l2_buffer bq;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int nplanes = 2, i;
    unsigned produced = 0, enc_fail = 0, nfrm = 0;
    uint64_t t0, t1, tacc[5] = {0};

    if (fd < 0)
        die("open /dev/video0");
    if (ioctl(fd, VIDIOC_S_INPUT, &in) < 0)
        die("S_INPUT");

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix_mp.width = g_w;
    fmt.fmt.pix_mp.height = g_h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV21M;   /* 双平面, Y/C 分开 */
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 2;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
        die("S_FMT");
    g_w = fmt.fmt.pix_mp.width;
    g_h = fmt.fmt.pix_mp.height;
    nplanes = fmt.fmt.pix_mp.num_planes;
    printf("capture: %ux%u planes=%d stride=%u\n", g_w, g_h, nplanes,
           fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

    memset(&rb, 0, sizeof(rb));
    rb.count = NSLOT;
    rb.type = type;
    rb.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0)
        die("REQBUFS");
    printf("capture: reqbufs %u USERPTR buffers\n", rb.count);
    mem_stat("after reqbufs");

    for (i = 0; i < NSLOT; i++) {
        memset(&bq, 0, sizeof(bq));
        memset(planes, 0, sizeof(planes));
        bq.type = type;
        bq.memory = V4L2_MEMORY_USERPTR;
        bq.index = i;
        bq.m.planes = planes;
        bq.length = nplanes;
        planes[0].m.userptr = (unsigned long)g_vin[i].pAddrVirY;
        planes[0].length = g_w * g_h;
        planes[1].m.userptr = (unsigned long)g_vin[i].pAddrVirC;
        planes[1].length = g_w * g_h / 2;
        if (ioctl(fd, VIDIOC_QBUF, &bq) < 0)
            die("QBUF(userptr)");
    }

    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
        die("STREAMON");
    printf("capture: streaming %d buffers\n", NSLOT);
    mem_stat("after streamon");
    t0 = now_us();

    while (g_running) {
        fd_set fds;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int sr;

        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        sr = select(fd + 1, &fds, NULL, NULL, &tv);
        if (sr <= 0) {
            t1 = now_us();
            if (t1 - t0 >= 5000000ull) {
                printf("enc: select=%d, %u captured, %u encoded, %u fail\n",
                       sr, nfrm, produced, enc_fail);
                mem_stat("running");
                t0 = t1;
            }
            continue;
        }

        memset(&bq, 0, sizeof(bq));
        memset(planes, 0, sizeof(planes));
        bq.type = type;
        bq.memory = V4L2_MEMORY_USERPTR;
        bq.m.planes = planes;
        bq.length = nplanes;
        if (ioctl(fd, VIDIOC_DQBUF, &bq) < 0) {
            if (errno != EAGAIN)
                perror("DQBUF");
            continue;
        }
        nfrm++;

        if (getenv("MQ_DUMP_YUV") && nfrm == 3) {
            int f = open("/tmp/dump.yuv", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (f >= 0) {
                write(f, g_vin[bq.index].pAddrVirY, g_w * g_h);
                write(f, g_vin[bq.index].pAddrVirC, g_w * g_h / 2);
                close(f);
                printf("dump: yuv %dx%d from slot %u\n", g_w, g_h, bq.index);
            }
        }

        {
            int idx = bq.index;
            VencInputBuffer *vin = &g_vin[idx];
            VencOutputBuffer vout;
            uint64_t ta = now_us(), tn;

            /* 输入数据已经在编码器缓冲里, 直接编码 */
            vin->bEnableCorp = 0;
            AddOneInputBuffer(enc, vin);
            tn = now_us(); tacc[0] += tn - ta; ta = tn;
            VideoEncodeOneFrame(enc);
            tn = now_us(); tacc[1] += tn - ta; ta = tn;
            AlreadyUsedInputBuffer(enc, vin);
            tn = now_us(); tacc[2] += tn - ta; ta = tn;

            if (GetOneBitstreamFrame(enc, &vout) == 0) {
                tn = now_us(); tacc[3] += tn - ta; ta = tn;
                size_t n = (size_t)vout.nSize0 + vout.nSize1;
                if (n > MAX_FRAME)
                    n = MAX_FRAME;
                pthread_mutex_lock(&g_flock);
                memcpy(g_js, vout.pData0, n < vout.nSize0 ? n : vout.nSize0);
                if (vout.nSize1 && n > vout.nSize0)
                    memcpy(g_js + vout.nSize0, vout.pData1, n - vout.nSize0);
                g_jlen = n;
                g_jseq++;
                pthread_cond_broadcast(&g_fcond);
                pthread_mutex_unlock(&g_flock);
                produced++;
                tn = now_us(); tacc[4] += tn - ta;
                static int dumped;
                if (getenv("MQ_DUMP") && !dumped) {
                    dumped = 1;
                    int f = open("/tmp/dump.jpg", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (f >= 0) {
                        write(f, vout.pData0, vout.nSize0);
                        if (vout.nSize1)
                            write(f, vout.pData1, vout.nSize1);
                        close(f);
                        printf("dump: wrote %zu B jpeg\n", n);
                    }
                }
                FreeOneBitStreamFrame(enc, &vout);
            } else {
                enc_fail++;
            }
        }

        /* USERPTR 重新入队: 沿用 DQBUF 返回的 bq, 并重置 userptr */
        bq.type = type;
        bq.memory = V4L2_MEMORY_USERPTR;
        bq.m.planes = planes;
        bq.length = nplanes;
        planes[0].m.userptr = (unsigned long)g_vin[bq.index].pAddrVirY;
        planes[0].length = g_w * g_h;
        planes[1].m.userptr = (unsigned long)g_vin[bq.index].pAddrVirC;
        planes[1].length = g_w * g_h / 2;
        if (ioctl(fd, VIDIOC_QBUF, &bq) < 0)
            printf("QBUF back fail errno=%d idx=%u\n", errno, bq.index);

        t1 = now_us();
        if (t1 - t0 >= 5000000ull) {
            double sec = (t1 - t0) / 1e6;
            printf("enc: %.1f fps, %u captured, %u encoded, %u fail\n"
                   "     us/frame: add %llu encode %llu ret %llu get %llu publish %llu\n",
                   nfrm / sec, nfrm, produced, enc_fail,
                   (unsigned long long)(produced ? tacc[0] / produced : 0),
                   (unsigned long long)(produced ? tacc[1] / produced : 0),
                   (unsigned long long)(produced ? tacc[2] / produced : 0),
                   (unsigned long long)(produced ? tacc[3] / produced : 0),
                   (unsigned long long)(produced ? tacc[4] / produced : 0));
            mem_stat("running");
            produced = 0;
            nfrm = 0;
            memset(tacc, 0, sizeof(tacc));
            t0 = t1;
        }
    }

    ioctl(fd, VIDIOC_STREAMOFF, &type);
    close(fd);
    return NULL;
}

/* ---------------- RTSP ---------------- */

static int hdr_get(const char *req, const char *name, char *out, int outsz)
{
    const char *p = req;
    size_t nl = strlen(name);

    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            int i = 0;
            while (*v == ' ' || *v == '\t') v++;
            while (*v && *v != '\r' && *v != '\n' && i < outsz - 1)
                out[i++] = *v++;
            out[i] = 0;
            return 0;
        }
    }
    return -1;
}

static void send_all(int fd, const char *s)
{
    size_t n = strlen(s), off = 0;
    while (off < n) {
        ssize_t r = send(fd, s + off, n - off, MSG_NOSIGNAL);
        if (r <= 0)
            return;
        off += r;
    }
}

static void send_resp(int fd, int cseq, const char *extra, const char *body,
                      const char *ctype)
{
    char hdr[2048];
    int n = snprintf(hdr, sizeof(hdr),
                     "RTSP/1.0 200 OK\r\n"
                     "CSeq: %d\r\n"
                     "Server: mq_rtsp/1.0\r\n"
                     "%s",
                     cseq, extra ? extra : "");
    if (n < 0 || n >= (int)sizeof(hdr))
        return;
    if (body) {
        int blen = strlen(body);
        snprintf(hdr + n, sizeof(hdr) - n,
                 "Content-Type: %s\r\nContent-Length: %d\r\n\r\n",
                 ctype ? ctype : "application/sdp", blen);
        send_all(fd, hdr);
        send_all(fd, body);
    } else {
        snprintf(hdr + n, sizeof(hdr) - n, "Content-Length: 0\r\n\r\n");
        send_all(fd, hdr);
    }
}

static void handle_client(int fd, struct sockaddr_in peer)
{
    char req[8192];
    size_t used = 0;

    while (g_running) {
        ssize_t r;
        char *end;

        r = recv(fd, req + used, sizeof(req) - 1 - used, 0);
        if (r <= 0)
            break;
        used += r;
        req[used] = 0;

        while ((end = strstr(req, "\r\n\r\n")) != NULL) {
            char method[16] = {0}, url[512] = {0}, cseq_s[32] = {0}, tr[256] = {0};
            int cseq = 0;
            size_t consumed;

            sscanf(req, "%15s %511s", method, url);
            hdr_get(req, "CSeq", cseq_s, sizeof(cseq_s));
            hdr_get(req, "Transport", tr, sizeof(tr));
            if (cseq_s[0])
                cseq = atoi(cseq_s);

            if (!strcmp(method, "OPTIONS")) {
                send_resp(fd, cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, "
                          "PAUSE, TEARDOWN, GET_PARAMETER\r\n", NULL, NULL);

            } else if (!strcmp(method, "DESCRIBE")) {
                char sdp[1024];
                snprintf(g_base_url, sizeof(g_base_url), "%.200s", url);
                snprintf(sdp, sizeof(sdp),
                         "v=0\r\n"
                         "o=- 0 0 IN IP4 0.0.0.0\r\n"
                         "s=MangoPi D1s OV5640 MJPEG\r\n"
                         "c=IN IP4 0.0.0.0\r\n"
                         "t=0 0\r\n"
                         "a=tool:mq_rtsp\r\n"
                         "a=control:*\r\n"
                         "m=video 0 RTP/AVP 26\r\n"
                         "a=rtpmap:26 JPEG/90000\r\n"
                         "a=framerate:%d\r\n"
                         "a=control:track1\r\n", g_fps);
                send_resp(fd, cseq, NULL, sdp, "application/sdp");
                printf("rtsp: DESCRIBE %s\n", url);

            } else if (!strcmp(method, "SETUP")) {
                char *cp = strstr(tr, "client_port=");
                int p1 = 0, p2 = 0;
                char extra[256];

                if (cp)
                    sscanf(cp + 12, "%d-%d", &p1, &p2);
                if (p2 == 0)
                    p2 = p1 + 1;
                if (p1 <= 0 || !strstr(tr, "RTP/AVP")) {
                    send_all(fd, "RTSP/1.0 461 Unsupported Transport\r\n"
                             "CSeq: 1\r\n\r\n");
                } else {
                    pthread_mutex_lock(&g_lock);
                    g_sess.addr = peer;
                    g_sess.addr.sin_port = htons(p1);   /* RTP 发往 client_port */
                    g_sess.port = p1;
                    pthread_mutex_unlock(&g_lock);
                    snprintf(extra, sizeof(extra),
                             "Transport: RTP/AVP;unicast;client_port=%d-%d;"
                             "server_port=%d-%d\r\n"
                             "Session: 12345678;timeout=60\r\n",
                             p1, p2, g_server_port, g_server_port + 1);
                    send_resp(fd, cseq, extra, NULL, NULL);
                    printf("rtsp: SETUP client_port=%d-%d\n", p1, p2);
                }

            } else if (!strcmp(method, "PLAY")) {
                char extra[512];
                snprintf(extra, sizeof(extra),
                         "Session: 12345678\r\n"
                         "Range: npt=0.000-\r\n"
                         "RTP-Info: url=%.200s/track1;seq=%u;rtptime=%u\r\n",
                         g_base_url, g_seq, g_ts);
                send_resp(fd, cseq, extra, NULL, NULL);
                pthread_mutex_lock(&g_lock);
                g_sess.playing = 1;
                pthread_mutex_unlock(&g_lock);
                printf("rtsp: PLAY\n");

            } else if (!strcmp(method, "PAUSE") || !strcmp(method, "TEARDOWN")) {
                pthread_mutex_lock(&g_lock);
                g_sess.playing = 0;
                pthread_mutex_unlock(&g_lock);
                send_resp(fd, cseq, "Session: 12345678\r\n", NULL, NULL);
                printf("rtsp: %s\n", method);

            } else if (!strcmp(method, "GET_PARAMETER") ||
                       !strcmp(method, "SET_PARAMETER")) {
                send_resp(fd, cseq, "Session: 12345678\r\n", NULL, NULL);

            } else {
                send_all(fd, "RTSP/1.0 405 Method Not Allowed\r\n"
                         "CSeq: 1\r\n\r\n");
            }

            consumed = (end - req) + 4;
            memmove(req, req + consumed, used - consumed);
            used -= consumed;
            req[used] = 0;
        }
        if (used >= sizeof(req) - 1)
            used = 0;
    }
    pthread_mutex_lock(&g_lock);
    g_sess.playing = 0;
    pthread_mutex_unlock(&g_lock);
}

int main(int argc, char **argv)
{
    pthread_t th_cap, th_rtp;
    pthread_attr_t at;
    VideoEncoder *enc;
    int lfd, one = 1;
    struct sockaddr_in addr;

    if (argc > 1) g_bitrate_kbps = atoi(argv[1]);
    if (argc > 2) g_quality = atoi(argv[2]);
    if (argc > 3) g_port = atoi(argv[3]);
    if (argc > 4) g_w = atoi(argv[4]);
    if (argc > 5) g_h = atoi(argv[5]);

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    {
        void on_crash(int sig) {
            char b[] = "mq_rtsp: fatal signal\n";
            (void)sig;
            write(2, b, sizeof(b) - 1);
            _exit(139);
        }
        signal(SIGSEGV, on_crash);
        signal(SIGBUS, on_crash);
        signal(SIGILL, on_crash);
    }

    /* 内存紧张时让内核优先杀本进程, 保住 dropbear/网络 */
    {
        int f = open("/proc/self/oom_score_adj", O_WRONLY);
        if (f >= 0) {
            write(f, "500", 3);
            close(f);
        }
    }

    /* 以 MQ_DAEMON=1 启动时脱离 ssh 会话 */
    if (getenv("MQ_DAEMON"))
        daemon(0, 1);

    g_js = malloc(MAX_FRAME);
    g_rtp_buf = malloc(MAX_FRAME);
    if (!g_js || !g_rtp_buf)
        die("malloc");

    enc = venc_open();
    if (!enc)
        return 1;

    g_rtp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_rtp_fd < 0)
        die("socket udp");
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_rtp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind udp");
    {
        socklen_t sl = sizeof(addr);
        getsockname(g_rtp_fd, (struct sockaddr *)&addr, &sl);
        g_server_port = ntohs(addr.sin_port);
    }
    fcntl(g_rtp_fd, F_SETFL, O_NONBLOCK);

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0)
        die("socket tcp");
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(g_port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind tcp");
    listen(lfd, 4);

    printf("mq_rtsp: rtsp://<board-ip>:%d/stream  (RTP port %d)\n",
           g_port, g_server_port);
    mem_stat("after venc");

    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, THREAD_STACK);
    pthread_create(&th_cap, &at, capture_thread, enc);
    pthread_create(&th_rtp, &at, rtp_thread, NULL);
    pthread_attr_destroy(&at);

    while (g_running) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr *)&peer, &pl);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            usleep(100000);
            continue;
        }
        printf("rtsp: client %s:%d\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        pthread_mutex_lock(&g_lock);
        g_sess.addr = peer;
        g_sess.port = 0;
        pthread_mutex_unlock(&g_lock);
        handle_client(cfd, peer);
        close(cfd);
        printf("rtsp: client closed\n");
    }
    return 0;
}
