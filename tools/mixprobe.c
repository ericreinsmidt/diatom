/* Can a port set an ALSA mixer control with no alsa-lib and no fork?
 *
 * The Brick's sysroot carries SDL2 and nothing else, so `port/brick.c` cannot
 * link alsa-lib or tinyalsa, and shelling out to `tinymix` per keypress means
 * a process spawn inside the input path plus a dependency on a firmware binary.
 * The third option is the one the port already uses for video: a raw ioctl on
 * a device node. brick.c drives /dev/fb0 that way; /dev/snd/controlC0 is the
 * same pattern.
 *
 * The risk is the struct ABI. `snd_ctl_elem_value`'s SIZE is encoded into the
 * ioctl number by _IOWR, so a wrong layout produces a wrong request number and
 * the call fails - or worse, hits a different command. This exists to prove the
 * layout against the running kernel before any of it reaches the port.
 *
 *   mixprobe                     read 'digital volume'
 *   mixprobe <n>                 set it, read it back
 *
 * An instrument. See tools/README.md.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdint.h>

/* From the kernel UAPI (sound/asound.h), vendored rather than depended on.
 * Only the integer case is needed; the union is declared at full size because
 * its size is what _IOWR bakes into the request number. */
struct dm_ctl_elem_id {
	unsigned int  numid;
	int           iface;
	unsigned int  device;
	unsigned int  subdevice;
	unsigned char name[44];
	unsigned int  index;
};

struct dm_aes_iec958 {
	unsigned char status[24];
	unsigned char subcode[147];
	unsigned char pad;
	unsigned char dig_subframe[4];
};

struct dm_ctl_elem_value {
	struct dm_ctl_elem_id id;
	unsigned int indirect: 1;
	union {
		union { long value[128]; long *value_ptr; } integer;
		union { long long value[64]; long long *value_ptr; } integer64;
		union { unsigned int item[128]; unsigned int *item_ptr; } enumerated;
		union { unsigned char data[512]; unsigned char *data_ptr; } bytes;
		struct dm_aes_iec958 iec958;
	} value;
	unsigned char reserved[128];
};

#define DM_CTL_ELEM_READ   _IOWR('U', 0x12, struct dm_ctl_elem_value)
#define DM_CTL_ELEM_WRITE  _IOWR('U', 0x13, struct dm_ctl_elem_value)
#define DM_CTL_IFACE_MIXER 2

static int elem(int fd, const char *name, long *val, int write)
{
	struct dm_ctl_elem_value v;
	memset(&v, 0, sizeof v);
	v.id.iface = DM_CTL_IFACE_MIXER;
	snprintf((char *)v.id.name, sizeof v.id.name, "%s", name);
	if (write) {
		v.value.integer.value[0] = *val;
		if (ioctl(fd, DM_CTL_ELEM_WRITE, &v) < 0) return -1;
	} else {
		if (ioctl(fd, DM_CTL_ELEM_READ, &v) < 0) return -1;
		*val = v.value.integer.value[0];
	}
	return 0;
}

int main(int argc, char **argv)
{
	int fd = open("/dev/snd/controlC0", O_RDWR);
	long v = 0;

	printf("sizeof(elem_value) = %zu\n", sizeof(struct dm_ctl_elem_value));
	printf("READ  request = 0x%lx\n", (unsigned long)DM_CTL_ELEM_READ);
	printf("WRITE request = 0x%lx\n", (unsigned long)DM_CTL_ELEM_WRITE);

	if (fd < 0) { perror("open /dev/snd/controlC0"); return 2; }

	if (elem(fd, "digital volume", &v, 0) < 0) { perror("read"); return 1; }
	printf("digital volume = %ld\n", v);

	if (argc > 1) {
		v = strtol(argv[1], NULL, 10);
		if (elem(fd, "digital volume", &v, 1) < 0) { perror("write"); return 1; }
		v = 0;
		if (elem(fd, "digital volume", &v, 0) < 0) { perror("re-read"); return 1; }
		printf("after write   = %ld\n", v);
	}
	close(fd);
	return 0;
}
