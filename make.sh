gzip -nc ccd.html | ../thermostat/bin2c html > ccd.html.c
gcc -o currentcostd -s -pie -fwhole-program -flto cc.c ccd.c -O3 -Wall -std=gnu99 -D_GNU_SOURCE -lexpat -levent
#clang -o currentcostd -s cc.c ccd.c -O3 -Wall -std=gnu99 -D_GNU_SOURCE -lexpat -levent
#gcc -o currentcostd cc.c ccd.c -ggdb -Wall -std=gnu99 -D_GNU_SOURCE -I/home/sean/build/include -L/home/sean/build/lib -lexpat -levent
