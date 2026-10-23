CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -I../main

.PHONY: test clean

test: test_printer_id
	./test_printer_id

test_printer_id: test_printer_id.c ../main/printer_id.c ../main/printer_id.h
	$(CC) $(CFLAGS) -o $@ test_printer_id.c ../main/printer_id.c

clean:
	rm -f test_printer_id
