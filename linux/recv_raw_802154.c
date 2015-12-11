#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/ether.h>

#define DEFAULT_IF	"wpan0"
#define BUF_SIZ		1024

int main(int argc, char *argv[]) {
	char sender[INET6_ADDRSTRLEN];
	int sockfd, i;
	int sockopt;
	ssize_t numbytes;
	struct ifreq ifopts;	/* set promiscuous mode */
	uint8_t buf[BUF_SIZ];
	char ifName[IFNAMSIZ];

	// Get interface name
	if (argc > 1) {
		strcpy(ifName, argv[1]);
	} else {
		strcpy(ifName, DEFAULT_IF);
	}

	// Open PF_PACKET socket, listening for EtherType ETH_P_IEEE802154
	if ((sockfd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_IEEE802154))) == -1) {
		perror("listener: socket");
		return -1;
	}

	// Set interface to promiscuous mode - do we need to do this every time?
	strncpy(ifopts.ifr_name, ifName, IFNAMSIZ-1);
	ioctl(sockfd, SIOCGIFFLAGS, &ifopts);
	ifopts.ifr_flags |= IFF_PROMISC;
	ioctl(sockfd, SIOCSIFFLAGS, &ifopts);
	// Allow the socket to be reused - incase connection is closed prematurely
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof sockopt) == -1) {
		perror("setsockopt");
		close(sockfd);
		exit(EXIT_FAILURE);
	}
	// Bind to device
	if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, ifName, IFNAMSIZ-1) == -1)	{
		perror("SO_BINDTODEVICE");
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	// Wait for data
	while (1) {
		printf("listener: Waiting to recvfrom...\n");
		numbytes = recvfrom(sockfd, buf, BUF_SIZ, 0, NULL, NULL);
		printf("listener: got packet %lu bytes\n", numbytes);

		// Print packet bytes
		for (i=0; i<numbytes; i++) {
			printf("%02x", buf[i]);
		}
		printf("\n");
	}

	close(sockfd);
	return 0;
}
