#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <net/if.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <linux/if_tun.h>
#include <linux/ioctl.h>
#include <linux/sockios.h>
// #include <linux/ipv6.h>
// #include <linux/in6.h>
#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <asm/byteorder.h>

#include "debug.h"

#define TUNNEL_SERVER_LISTEN_PORT 32100
#define MAXEVENTS 64

struct ifreq6 {
  struct in6_addr addr;
  uint32_t prefix_len;
  unsigned int ifindex;
};

// // The prefix that all of the /60s come from
// #define SLASH_48 "2001:db8:543:0::0"
// // The global IP addresses for each client
// #define SLASH_64 "2607:f017:999:1::0"

// Data structure to hold all routing information for each connected client
struct socket_prefix {
  struct in6_addr addr_pd;  // The /60 (or whatever) that was assigned to this client. pd == prefix delegation
  uint32_t        plen_pd;  // 0 if not set, 60 (or the actual prefix) if set
  struct in6_addr addr_ll;  // The link local assigned to this client
  uint32_t        plen_ll;  // 0 if not set, 128 if valid
};


struct ipv6hdr {
 #if defined(__LITTLE_ENDIAN_BITFIELD)
         __u8                    priority:4,
                                 version:4;
 #elif defined(__BIG_ENDIAN_BITFIELD)
         __u8                    version:4,
                                 priority:4;
 #else
 #error  "Please fix <asm/byteorder.h>"
 #endif
         uint8_t                    flow_lbl[3];

         uint16_t                  payload_len;
         uint8_t                    nexthdr;
         uint8_t                    hop_limit;

         struct  in6_addr        saddr;
         struct  in6_addr        daddr;
 };


// Makes the given file descriptor non-blocking.
// Returns 0 on success, -1 on failure.
int make_nonblocking (int fd) {
  int flags, ret;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  // Set the nonblocking flag.
  flags |= O_NONBLOCK;
  ret = fcntl(fd, F_SETFL, flags);
  if (ret == -1) {
    return -1;
  }

  return 0;
}


static int create_and_bind () {
  struct addrinfo hints;
  struct addrinfo *result;
  struct addrinfo *rp;
  int s;
  int sfd;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM; // tcp
  hints.ai_flags    = AI_PASSIVE;

  s = getaddrinfo(NULL, "32100", &hints, &result);
  if (s != 0) {
    ERROR("Could not getaddrinfo.\n");
    return -1;
  }

  for (rp = result; rp != NULL; rp = rp->ai_next) {
    sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sfd == -1) {
      continue;
    }

    s = bind(sfd, rp->ai_addr, rp->ai_addrlen);
    if (s == 0) {
      // We managed to bind successfully!
      break;
    }

    close(sfd);
  }

  if (rp == NULL) {
    ERROR("Could not bind\n");
    return -1;
  }

  freeaddrinfo(result);

  return sfd;
}

void print_in6addr (struct in6_addr* a) {
  char str[INET6_ADDRSTRLEN];
  const char* ptr;
  ptr = inet_ntop(AF_INET6, a->s6_addr, str, INET6_ADDRSTRLEN);
  if (ptr == NULL) {
    perror("inet ntop");
  }
  printf("%s\n", str);
  // printf("%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
  //   a->s6_addr[0],a->s6_addr[1], a->s6_addr[2], a->s6_addr[3], a->s6_addr[4],
  //   a->s6_addr[5], a->s6_addr[6], a->s6_addr[7],
  //   a->s6_addr[8],a->s6_addr[9], a->s6_addr[10], a->s6_addr[11], a->s6_addr[12],
  //   a->s6_addr[13], a->s6_addr[14], a->s6_addr[15]);
}

static int parse_prefixes (struct socket_prefix* prefixes) {

  FILE* prefix_file;
  int read_len;
  int err;

  prefix_file = fopen("/etc/dibbler/client_assignments", "r");
  if (!prefix_file) {
    ERROR("Could not open file with prefixes.\n");
    ERROR("Does it exist?.\n");
    exit(1);
  }


  // Each line is:
  // fe80::1:2:3:4 2607:4::/60
  while (1) {
    char line[512];
    char* ptr;
    char* ptr2;
    int passes = 0;
    struct in6_addr local;
    struct in6_addr block;
    int match = 0;


    fgets(line, 512, prefix_file);

    if (feof(prefix_file)) {
      break;
    }


    ptr = line;
    while (1) {
      if (*ptr == '\0') {
        break;
      }
      if (passes == 0) {
        if (*ptr == ' ') {
          *ptr = '\0';
          ptr2 = ptr+1;
          err = inet_pton(AF_INET6, line, &local);
          if (err == 0) {
            printf("could not convert %s\n", line);
          }
          passes++;
        }
      } else {
        if (*ptr == '\n') {
          *ptr = '\0';
          *(ptr-1) = '0';
          *(ptr-2) = '0';
          *(ptr-3) = '0';
          err = inet_pton(AF_INET6, ptr2, &block);
          if (err == 0) {
            printf("could not convert %s\n", ptr2);
          }
          match = 1;
          break;
        }
      }
      ptr++;
    }

    if (match) {

      printf("Got local and block\n");
      print_in6addr(&local);
      print_in6addr(&block);

      // Got link local and the block.
      // Now match the link-local address and add the block.
      int j;
      for (j=0; j<512; j++) {
        struct socket_prefix* prefix = &prefixes[j];
        if (prefix->plen_ll != 0) {
          // Allow pd to be updated in case it changes
          printf("trying %i\n", j);

          // This could be the prefix match
          if (memcmp(prefix->addr_ll.s6_addr, local.s6_addr, 16) == 0) {
            // Found match!
            memcpy(prefix->addr_pd.s6_addr, block.s6_addr, 16);
            prefix->plen_pd = 62;
            printf("set %i as %02x\n", j, prefix->addr_pd.s6_addr[15]);
            print_in6addr(&prefix->addr_pd);
            break;
          }
        }
      }
    }

  }

  // read_len = read(macfile, macbuf, 128);
  // if (read_len < 0) {
  //  fprintf(stderr, "Could not read MAC address file.\n");
  //  return -1;
  // }
  close(prefix_file);
}


int main (int argc, char** argv) {
  struct ifreq ifr;
  struct ifreq6 ifr6;
  int err;
  char cmdbuf[4096];
  int sockfd;
  int tun_file;

  int sfd;
  int efd;
  struct epoll_event event;
  struct epoll_event *events;

  FILE* prefix_file;
  int num_valid_prefixes = 0;
  struct socket_prefix prefixes[512];

  struct in6_addr prefix_slash_52;
  struct in6_addr prefix_slash_64;

  if (argc != 3) {
    ERROR("usage: %s </52 full address> </64 full address>\n", argv[0]);
    exit(1);
  }


  // INIT
  memset(prefixes, 0, sizeof(prefixes));
  inet_pton(AF_INET6, argv[1], &prefix_slash_52);
  inet_pton(AF_INET6, argv[2], &prefix_slash_64);




  // Need to open a TUN device to get packets from linux and to send packets
  // to the kernel to be routed
  tun_file = open("/dev/net/tun", O_RDWR);
  if (tun_file < 0) {
    // error
    ERROR("Could not create a tun interface. errno: %i\n", errno);
    ERROR("%s\n", strerror(errno));
    exit(1);
  }

  // Clear the ifr struct
  memset(&ifr, 0, sizeof(ifr));

  // Set the TUN name
  strncpy(ifr.ifr_name, "ipv6-tun", IFNAMSIZ);

  // Select a TUN device
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

  // Make up a HW address


  // Setup the interface
  err = ioctl(tun_file, TUNSETIFF, (void *) &ifr);
  if (err < 0) {
    ERROR("ioctl could not set up tun interface\n");
    close(tun_file);
    exit(1);
  }

  // Make it persistent
  err = ioctl(tun_file, TUNSETPERSIST, 1);
  if (err < 0) {
    ERROR("Could not make persistent\n");
    exit(1);
  }

  // Make nonblocking in case select() gives us trouble
  make_nonblocking(tun_file);

  // Save the name of the tun interface
  // strncpy(tun_name, ifr.ifr_name, MAX_TUN_NAME_LEN);

  // Get a socket to perform the ioctls on
  sockfd = socket(AF_INET6, SOCK_DGRAM, 0);

  // Set the interface to be up
  // ifconfig tun0 up
  err = ioctl(sockfd, SIOCGIFFLAGS, &ifr);
  if (err < 0) {
    ERROR("ioctl could not get flags.\n");
    close(tun_file);
    exit(1);
  }
  ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
  err = ioctl(sockfd, SIOCSIFFLAGS, &ifr);
  if (err < 0) {
    ERROR("ioctl could not bring up the TUN network interface.\n");
    perror("tup up");
    ERROR("errno: %i\n", errno);
    close(tun_file);
    exit(1);
  }

  // Set the MTU of the interface
  // ifconfig tun0 mtu 1280
  ifr.ifr_mtu = 1280;
  err = ioctl(sockfd, SIOCSIFMTU, &ifr);
  if (err < 0) {
    ERROR("ioctl could not set the MTU of the TUN network interface.\n");
    close(tun_file);
    exit(1);
  }

  // Get the ifr_index
  err = ioctl(sockfd, SIOCGIFINDEX, &ifr);
  if (err < 0) {
    ERROR("ioctl could not get ifindex.\n");
    close(tun_file);
    exit(1);
  }

  // Set a fake HW address
  // ifr.ifr_hwaddr.sa_data[0] = 0xc0;
  // ifr.ifr_hwaddr.sa_data[1] = 0x98;
  // ifr.ifr_hwaddr.sa_data[2] = 0xe5;
  // ifr.ifr_hwaddr.sa_data[3] = 0xaa;
  // ifr.ifr_hwaddr.sa_data[4] = 0x11;
  // ifr.ifr_hwaddr.sa_data[5] = 0xbb;
  // err = ioctl(sockfd, SIOCSIFHWADDR, &ifr);
  // if (err < 0) {
  //   ERROR("Could not set hardware address.\n");
  //   perror("SIOCSIFHWADDR");
  //   close(tun_file);
  //   exit(1);
  // }

  // Set a dummy link-local address on the interface
  //ifconfig tun0 inet6 add fe80::212:aaaa:bbbb:ffff/64
  inet_pton(AF_INET6, "fe80::c298:e5ff:feaa:11bb", &ifr6.addr);
  ifr6.prefix_len = 64;
  ifr6.ifindex = ifr.ifr_ifindex;
  err = ioctl(sockfd, SIOCSIFADDR, &ifr6);
  if (err < 0) {
    ERROR("ioctl could not set link-local address TUN network interface.\n");
    ERROR("perhaps it was already set\n");
    // close(tun_file);
    // exit(1);
  }

  close(sockfd);




  // SETUP TCP KEEP-ALIVES SO THAT WE KNOW WHEN A CLIENT DISCONNECTS SOONER

  int kafd;
  kafd = open("/proc/sys/net/ipv4/tcp_keepalive_time", O_WRONLY);
  if (kafd < 0) {
    ERROR("Could not open keepalive time\n");
    exit(1);
  }
  err = write(kafd, "60", 2);
  if (err == -1) {
    ERROR("Could not write keepalive time\n");
    perror("keepalive_time");
    exit(1);
  }
  close(kafd);

  kafd = open("/proc/sys/net/ipv4/tcp_keepalive_intvl", O_WRONLY);
  if (kafd < 0) {
    ERROR("Could not open keepalive interval\n");
    exit(1);
  }
  err = write(kafd, "20", 2);
  if (err == -1) {
    ERROR("Could not write keepalive interval\n");
    perror("keepalive_intvl");
    exit(1);
  }
  close(kafd);

  kafd = open("/proc/sys/net/ipv4/tcp_keepalive_probes", O_WRONLY);
  if (kafd < 0) {
    ERROR("Could not open keepalive probes\n");
    exit(1);
  }
  err = write(kafd, "3", 1);
  if (err == -1) {
    ERROR("Could not write keepalive probes\n");
    perror("keepalive_probes");
    exit(1);
  }
  close(kafd);





  // ACCEPT TCP CONNECTIONS FOR TUNNEL CLIENTS

  // Create a socket
  sfd = create_and_bind();
  if (sfd == -1) {
    ERROR("Could not create a socket\n");
    exit(1);
  }

  err = make_nonblocking(sfd);
  if (err == -1) {
    ERROR("Could not make socket nonblocking\n");
    exit(1);
  }

  err = listen(sfd, SOMAXCONN);
  if (err == -1) {
    ERROR("Could not listen on socket.\n");
    perror("listen");
    exit(1);
  }

  efd = epoll_create1(0);
  if (efd == -1) {
    perror("epoll_create");
    exit(1);
  }

  // Add the TCP socket to epoll
  event.data.fd = sfd;
  event.events = EPOLLIN | EPOLLET;
  err = epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &event);
  if (err == -1) {
    perror("epoll_ctl - TCP");
    exit(1);
  }

  // Add the TUN device to epoll
  event.data.fd = tun_file;
  event.events = EPOLLIN | EPOLLET;
  err = epoll_ctl(efd, EPOLL_CTL_ADD, tun_file, &event);
  if (err == -1) {
    perror("epoll_ctl - TUN");
    exit(1);
  }

  // Buffer where events are returned
  events = calloc(MAXEVENTS, sizeof(event));




  // The event loop
  while (1) {
    int n, i;

    // Wait for something to happen on one of the file descriptors
    // we are waiting on
    n = epoll_wait(efd, events, MAXEVENTS, -1);

    // Iterate all of the active events
    for (i = 0; i < n; i++) {

      // Check that everything is kosher
      if ((events[i].events & EPOLLERR) ||
          (events[i].events & EPOLLHUP) ||
          (!(events[i].events & EPOLLIN))) {
        // An error has occured on this fd, or the socket is not
        // ready for reading (why were we notified then?)
        ERROR("epoll error\n");
        continue;

      // Check if this is a new connection on the TCP listening socket
      } else if (events[i].data.fd == sfd) {
        while (1) {
          struct sockaddr in_addr;
          socklen_t in_len;
          int infd;
          char hbuf[NI_MAXHOST];
          char sbuf[NI_MAXSERV];

          in_len = sizeof(in_addr);
          infd = accept(sfd, &in_addr, &in_len);
          if (infd == -1) {
            if ((errno == EAGAIN) ||
              (errno == EWOULDBLOCK)) {
              // We have processed all incoming connections.
              break;
            } else {
              perror("accept");
              break;
            }
          }

          err = getnameinfo(&in_addr,
                          in_len,
                          hbuf,
                          sizeof(hbuf),
                          sbuf,
                          sizeof(sbuf),
                          NI_NUMERICHOST | NI_NUMERICSERV);
          if (err == 0) {
            DBG("Accepted connection on descriptor %d "
                   "(host=%s, port=%s)\n", infd, hbuf, sbuf);
          }

          // Make the incoming socket non-blocking and add it to the
          // list of fds to monitor.
          err = make_nonblocking(infd);
          if (err == -1) {
            ERROR("Could not make nonblocking\n");
            exit(1);
          }

          event.data.fd = infd;
          event.events  = EPOLLIN | EPOLLET;
          err = epoll_ctl(efd, EPOLL_CTL_ADD, infd, &event);
          if (err == -1) {
            perror ("epoll_ctl");
            exit(1);
          }
        }

        continue;

      // Check if we got data on the tun device
      // } else if (events[i].data.fd == tun_file) {

      // }

      } else {
        // Some socket has data ready. Read it all and handle it.

        int done = 0;

        // Loop until we have all of the data
        while (1) {
          ssize_t count;
          char buf[4096];

          count = read(events[i].data.fd, buf, sizeof(buf));
          if (count == -1){
            // If errno == EAGAIN, that means we have read all
            // data. So go back to the main loop.
            if (errno != EAGAIN) {
              perror ("read");
              done = 1;
            }
            break;
          } else if (count == 0) {
            // End of file. The remote has closed the
            // connection.
            done = 1;
            break;
          }

          if (events[i].data.fd == tun_file) {
            // Got a packet from the TUN device. Now we have to figure out
            // which TCP socket to send it to.
            printf("GOT DATA FROM TUN\n");

            struct ipv6hdr *iph = (struct ipv6hdr*) buf;

            if (iph->version == 6) {

              // Check to see if the packet is in the /52 that this tunnel
              // is responsible for
              if (memcmp(iph->daddr.s6_addr, prefix_slash_52.s6_addr, 6) == 0 &&
                  ((iph->daddr.s6_addr[6] & 0xf0) == (prefix_slash_52.s6_addr[6] & 0xf0))) {
                printf("This dest in /52\n");

                // This is in the /52
                // Need to find a match for this destination
                int j;
                int match = 0;
                for (j=0; j<512; j++) {
                  if (prefixes[j].plen_pd != 0 &&
                      memcmp(iph->daddr.s6_addr, prefixes[j].addr_pd.s6_addr, 7) == 0 &&
                      ((iph->daddr.s6_addr[7] & 0xfc) == (prefixes[j].addr_pd.s6_addr[7] & 0xfc))) {
                    // Found match
                    printf("Found destination with matching /62.\n");
                    err = write(j, buf, count);
                    match = 1;
                    break;
                  }
                }

                if (!match) {
                  printf("Could not find match originally. Reading assignments\n");
                  parse_prefixes(prefixes);

                  // Check for matches again
                  for (j=0; j<512; j++) {
                    if (prefixes[j].plen_pd != 0 &&
                        memcmp(iph->daddr.s6_addr, prefixes[j].addr_pd.s6_addr, 7) == 0 &&
                        ((iph->daddr.s6_addr[7] & 0xfc) == (prefixes[j].addr_pd.s6_addr[7] & 0xfc))) {
                      // Found match
                      printf("Found destination with matching /62 after loading prefixes.\n");
                      err = write(j, buf, count);
                      break;
                    }
                  }
                }

              // Check to see if destination is in /64 or ll
              } else if (memcmp(iph->daddr.s6_addr, prefix_slash_64.s6_addr, 8) == 0 ||
                         (iph->daddr.s6_addr[0] == 0xfe &&
                          iph->daddr.s6_addr[1] == 0x80)) {
                printf("This dest in /64 or link local\n");

                // Need to find a match for this destination
                int j=0;
                for (j=0; j<512; j++) {
                  if (prefixes[j].plen_ll != 0) {
                    printf("checking tcp %i\n", j);
                    printf("%02x%02x%02x%02x%02x%02x%02x%02x\n", iph->daddr.s6_addr[8],
                      iph->daddr.s6_addr[9], iph->daddr.s6_addr[10], iph->daddr.s6_addr[11], iph->daddr.s6_addr[12],
                      iph->daddr.s6_addr[13], iph->daddr.s6_addr[14], iph->daddr.s6_addr[15]);
                    printf("%02x%02x%02x%02x%02x%02x%02x%02x\n", prefixes[j].addr_ll.s6_addr[8],
                      prefixes[j].addr_ll.s6_addr[9], prefixes[j].addr_ll.s6_addr[10], prefixes[j].addr_ll.s6_addr[11], prefixes[j].addr_ll.s6_addr[12],
                      prefixes[j].addr_ll.s6_addr[13], prefixes[j].addr_ll.s6_addr[14], prefixes[j].addr_ll.s6_addr[15]);
                  }
                  if (prefixes[j].plen_ll != 0 &&
                      memcmp(iph->daddr.s6_addr+8, prefixes[j].addr_ll.s6_addr+8, 8) == 0) {
                    // Found match
                    printf("Found TCP destination for packet.\n");
                    err = write(j, buf, count);
                    break;
                  }
                }


              } else {
                printf("some other packet, can't forward\n");
              }
            }

          } else {
            // Got this from one of the TCP connections

            if (events[i].data.fd < 512) {
              // Not outside the bounds
              struct socket_prefix* prefix = &prefixes[events[i].data.fd];

              // Check to see if we know the link local address of this client.
              if (prefix->plen_ll == 0) {
                // We do not know the link local address of this node. That is
                // bad, because we will not know how to route to it when we get
                // packets for it.

                // Inspect the packet to determine the IPv6 source address
                struct ipv6hdr *iph = (struct ipv6hdr*) buf;

                if (iph->version == 6) {
                  // OK good, got IPv6 packet

                  if (iph->saddr.s6_addr[0] == 0xfe &&
                      iph->saddr.s6_addr[1] == 0x80) {
                    // This came from link-local address. Great.
                    // Save this
                    memcpy(&prefix->addr_ll, &iph->saddr, sizeof(struct in6_addr));
                    prefix->plen_ll = 128;
                  }
                }
              }
            }


//6 00 00000 0024 00 01 fe800000000000000000000099990001 ff0200000000000000000000000000163a000502000001008f00d66e0000000104000000ff020000000000000000000000000002




              int arghhh;
              for (arghhh = 0; arghhh < count; ++arghhh)
              {
                 // code
                printf("%02x", buf[arghhh]);
              }
              printf("\n");


            // }



            // Now dump it to the TUN device
            err = write(tun_file, buf, count);

            printf("wrote to tun\n");

          }


          /* Write the buffer to standard output */
          // err = write(1, buf, count);
          // if (err == -1) {
          //   perror ("write");
          //   abort ();
          // }
        }

        if (done) {
            printf("CLOSED\n");
            printf("  descriptor %d\n", events[i].data.fd);
            printf("  ");
            print_in6addr(&prefixes[events[i].data.fd].addr_ll);
            printf("  ");
            print_in6addr(&prefixes[events[i].data.fd].addr_pd);

            // Closing the descriptor will make epoll remove it
            // from the set of descriptors which are monitored.
            close (events[i].data.fd);
            prefixes[events[i].data.fd].plen_ll = 0;
            prefixes[events[i].data.fd].plen_pd = 0;
          }
      }
    }
  }

  free (events);

  close (sfd);
  close (tun_file);

  return EXIT_SUCCESS;
}



  // Register the file descriptor with the IO manager that will call select()
  // call IO.registerFileDescriptor(tun_file);

    // while (1);
// }

// Runs a command on the local system using
// the kernel command interpreter.
// int ssystem(const char *fmt, ...) {
//   char cmd[128];
//   va_list ap;
//   va_start(ap, fmt);
//   vsnprintf(cmd, sizeof(cmd), fmt, ap);
//   va_end(ap);
//   DBG("%s\n", cmd);
//   return system(cmd);
// }
