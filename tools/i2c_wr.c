/* i2c_wr <reg_hex> <val_hex> : 写 OV5640 寄存器 (bus 2, addr 0x3C) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

int main(int argc, char **argv)
{
    unsigned reg, val;
    unsigned char buf[3];
    int fd;

    if (argc < 3) {
        printf("usage: i2c_wr <reg_hex> <val_hex>\n");
        return 1;
    }
    reg = strtoul(argv[1], NULL, 16);
    val = strtoul(argv[2], NULL, 16);

    fd = open("/dev/i2c-2", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, I2C_SLAVE_FORCE, 0x3c) < 0) { perror("I2C_SLAVE"); return 1; }

    buf[0] = reg >> 8;
    buf[1] = reg & 0xff;
    buf[2] = val & 0xff;
    if (write(fd, buf, 3) != 3) { perror("write"); return 1; }

    /* 读回: 先写地址, 再读 1 字节 */
    buf[0] = reg >> 8;
    buf[1] = reg & 0xff;
    if (write(fd, buf, 2) != 2) { perror("write addr"); return 1; }
    unsigned char rb = 0;
    if (read(fd, &rb, 1) != 1) { perror("read"); return 1; }
    printf("reg 0x%04x = 0x%02x (wrote 0x%02x)\n", reg, rb, val);
    close(fd);
    return 0;
}
