#
# Makefile for C track, assignment 8.
#

CC     = gcc
CFLAGS = -g -Wall -m32

all:
	$(CC) $(CFLAGS) navm.c -o navm

clean:
	rm -f *.o bci 





