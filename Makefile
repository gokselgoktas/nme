CC = clang
MKDIR = mkdir

CFLAGS += -std=c17 -O3 -Wall -Werror

SRC = ./src/nme.c
TARGET = ./bin/nme.exe

all: $(TARGET)

$(TARGET): $(SRC)
	-@$(MKDIR) -p ./bin
	@$(CC) $(CFLAGS) -o $(TARGET) $^ $(LFLAGS)
