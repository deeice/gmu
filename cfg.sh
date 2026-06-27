LFLAGS="-L/usr/lib -L/usr/local/lib -pthread" \
CFLAGS="-march=armv5te -mtune=xscale -Os -pthread \
-I/usr/local/include -I/usr/local/include/ncurses -I/usr/local/include/opus" \
	   ./configure
