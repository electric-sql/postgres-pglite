SRCS = pglitec.c pglitec_pipe.c
OBJS = $(SRCS:.c=.o)
LIB  = libpglitec.a

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -static -fpic -o $@ -c $<

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
