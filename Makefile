MAKEFLAGS="-B -s"

main: 
	cc -std=gnu99 -pthread -lasound main.c -o main `pkg-config --cflags --libs gtk+-2.0`

lock_main: 
	cc -std=gnu99 -pthread -lasound lock_main.c -o lock_main `pkg-config --cflags --libs gtk+-2.0`
fut_main: 
	cc -std=gnu99 -pthread -lasound fut_main.c -o fut_main `pkg-config --cflags --libs gtk+-2.0`

clean:
	rm -f lock_main fut_main main

