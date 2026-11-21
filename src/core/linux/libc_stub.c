/* The functions of the libc.so.6 stub; libc_stub.ver gives each its version. Only their names matter: the image links
   against the stub and the loader maps the real libc.so.6. */
void malloc(void) {}
void calloc(void) {}
void free(void) {}
void malloc_trim(void) {}
void getaddrinfo(void) {}
void freeaddrinfo(void) {}
void pthread_attr_init(void) {}
void pthread_attr_destroy(void) {}
void clock_gettime(void) {}
void dlopen(void) {}
void dlsym(void) {}
void dlclose(void) {}
void dlerror(void) {}
void pthread_create(void) {}
void pthread_join(void) {}
void pthread_detach(void) {}
void pthread_attr_setstacksize(void) {}
