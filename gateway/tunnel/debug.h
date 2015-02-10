#ifndef __DEBUG_H__
#define __DEBUG_H__


#define DBG(...)\
  do {\
    flockfile(stdout);\
    printf("%s:%d:\t", __FILE__, __LINE__);\
    printf(__VA_ARGS__);\
    funlockfile(stdout);\
  } while (0)

#define ERROR(...)\
  do {\
    flockfile(stderr);\
    fprintf(stderr, "%s:%d\t", __FILE__, __LINE__);\
    fprintf(stderr, __VA_ARGS__);\
    funlockfile(stderr);\
  } while (0)

#endif
