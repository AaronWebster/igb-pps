all:: main

main: ts2phc.c
	gcc -o ts2phc ts2phc.c -lm -lrt -D_GNU_SOURCE -DHAVE_CLOCK_ADJTIME 

 
clean:
	rm -f ts2phc ts2phc.o

