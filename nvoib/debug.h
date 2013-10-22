//#define DEBUG

#ifdef DEBUG
#define dprintf(...) \
            do { fprintf(stderr, ##__VA_ARGS__); } while (0)
#else
#define dprintf(...)
#endif
