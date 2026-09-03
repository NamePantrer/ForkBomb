CC = gcc
CFLAGS = -Wall -O2
SRC = src/lab_spawn.c
OUT = src/lab_spawn

.PHONY: all clean

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f $(OUT) $(OUT).exe
