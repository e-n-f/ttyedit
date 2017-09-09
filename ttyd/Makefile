CFLAGS = -g -I/home/eric/src-mod/include

OBJS = ttyd.o

ttyd: $(OBJS)
	$(CC) -o ttyd $(OBJS)

.c.o:
	$(CC) -c $(CFLAGS) $<

clean:
	rm -f ttyd *.o

dist: clean
	cd ..; tar vcf ttyd.tar ttyd
