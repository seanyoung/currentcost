gcc -o bin2c bin2c.c -O3 -Wall
gzip -nc ccd.html | ./bin2c html > ccd.html.c
gcc -o currentcostd -s -fwhole-program -flto cc.c ccd.c -O3 -Wall -std=gnu99 -D_GNU_SOURCE -lexpat -levent
