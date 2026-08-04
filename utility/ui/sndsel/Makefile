PD_INCLUDE = /usr/include/pd
CFLAGS = -Wall -O2 -fPIC -I$(PD_INCLUDE)

sndsel.pd_linux: sndsel.c
	$(CC) $(CFLAGS) -shared -o $@ $<

clean:
	rm -f sndsel.pd_linux

.PHONY: clean
