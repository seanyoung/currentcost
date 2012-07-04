./f2c < ccd.html > ccd.html.c
gcc -o currentcostd -s -fwhole-program -combine cc.c ccd.c -O3 -Wall -std=gnu99 -D_GNU_SOURCE -I/home/sean/build/include -L/home/sean/build/lib -lexpat -levent
#gcc -o currentcostd cc.c ccd.c -ggdb -Wall -std=gnu99 -D_GNU_SOURCE -I/home/sean/build/include -L/home/sean/build/lib -lexpat -levent
