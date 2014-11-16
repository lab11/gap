#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "../ioctl.h"
#include "../bcp.h"
#include <unistd.h>

int main(char ** argv, int argc)
{

	int result = 0;
	printf("Testing nrf51822 driver...\n");
	int file_desc;
	file_desc = open("/dev/nrf51822_1", O_RDWR);

	printf("Setting nRF51822 to send advertisements.\n");
	struct nrf51822_simple_command sc;
	sc.command = BCP_COMMAND_SNIFF_ADVERTISEMENTS;
	ioctl(file_desc, NRF51822_IOCTL_SIMPLE_COMMAND, &sc);

	int i = 0;

	char buf[256];
	char pbuf[1024];
	char *buf_ptr = NULL;

	while (true) {
		result = read(file_desc, buf, 127);

		for (i = 0; i < result; i++) {
			printf("%02x", buf[i]);
		}
		printf("  ");
		for (i = 0; i < result; i++) {
			if (buf[i] > 31 && buf[i] < 127) {
				printf("%c", buf[i]);
			} else {
				printf(".", buf[i]);
			}
		}
		printf("\n");

	}

	close(file_desc);
}
