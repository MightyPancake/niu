# Convenience only: niu itself builds with a single command, no make needed:
#     cc niu.c -o niu        (MSVC: cl niu.c)

CC ?= cc

niu: niu.c src/*.c
	$(CC) niu.c -o niu

test: niu
	bash tests/run.sh ./niu

clean:
	rm -f niu niu.exe *.obj

.PHONY: test clean
