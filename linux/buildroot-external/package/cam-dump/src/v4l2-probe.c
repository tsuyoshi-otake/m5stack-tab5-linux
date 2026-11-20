/* v4l2-probe: minimal /dev/video0 check for the Tab5 camera.
 * VIDIOC_QUERYCAP + VIDIOC_G_FMT + one read() frame, printed.
 * Needs the sensor initialized first: cam-dump -S & (holds the
 * rail open), then v4l2-probe.
 *
 *   usage: v4l2-probe [device] [out.raw]
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/video0";
	struct v4l2_capability cap;
	struct v4l2_fmtdesc fdesc;
	struct v4l2_format fmt;
	unsigned char *fb;
	ssize_t got = 0, r;
	unsigned long sum = 0;
	int fd, i, ofd = -1;

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		perror("open video");
		return 1;
	}
	memset(&cap, 0, sizeof(cap));
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("VIDIOC_QUERYCAP");
		return 1;
	}
	printf("driver=%s card=%s bus=%s caps=0x%x devcaps=0x%x\n",
	       cap.driver, cap.card, cap.bus_info,
	       cap.capabilities, cap.device_caps);

	memset(&fdesc, 0, sizeof(fdesc));
	fdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	while (!ioctl(fd, VIDIOC_ENUM_FMT, &fdesc)) {
		printf("fmt[%u]=%s 0x%x\n", fdesc.index,
		       fdesc.description, fdesc.pixelformat);
		fdesc.index++;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
		perror("VIDIOC_G_FMT");
		return 1;
	}
	printf("g_fmt %ux%u %.4s bpl=%u size=%u\n",
	       fmt.fmt.pix.width, fmt.fmt.pix.height,
	       (char *)&fmt.fmt.pix.pixelformat,
	       fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);

	fb = malloc(fmt.fmt.pix.sizeimage);
	if (!fb) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}
	while ((r = read(fd, fb + got, fmt.fmt.pix.sizeimage - got)) > 0)
		got += r;
	printf("read %zd bytes\n", got);
	if (got != (ssize_t)fmt.fmt.pix.sizeimage) {
		fprintf(stderr, "short frame\n");
		return 1;
	}
	for (i = 0; i < got; i++)
		sum += fb[i];
	printf("mean=%lu\n", sum / got);

	if (argc > 2) {
		ofd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (ofd < 0 || write(ofd, fb, got) != got) {
			perror("write out");
			return 1;
		}
	}
	return 0;
}
