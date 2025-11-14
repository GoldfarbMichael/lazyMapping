# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -O2
LDFLAGS = -lrt

# Directories
SRC_DIR = src
MASTIK_DIR = Mastik-main
MASTIK_INCLUDE = $(MASTIK_DIR)/mastik
MASTIK_SRC = $(MASTIK_DIR)/src

# Project name
TARGET = lazyMapping

# Source files (since main is in utils.c)
SOURCES = $(SRC_DIR)/main.c $(SRC_DIR)/utils.c

# Mastik source files
MASTIK_SOURCES = $(MASTIK_SRC)/cb.c \
                 $(MASTIK_SRC)/ff.c \
                 $(MASTIK_SRC)/fr.c \
                 $(MASTIK_SRC)/l2.c \
                 $(MASTIK_SRC)/l3.c \
                 $(MASTIK_SRC)/lx.c \
                 $(MASTIK_SRC)/mm.c \
                 $(MASTIK_SRC)/pda.c \
                 $(MASTIK_SRC)/symbol.c \
                 $(MASTIK_SRC)/synctrace.c \
                 $(MASTIK_SRC)/util.c

# Object files
OBJECTS = $(SOURCES:.c=.o)
MASTIK_OBJECTS = $(MASTIK_SOURCES:.c=.o)

# Include paths
INCLUDES = -I$(MASTIK_INCLUDE)

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJECTS) $(MASTIK_SRC)/libmastik.a
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS) $(MASTIK_SRC)/libmastik.a

# Build Mastik static library
$(MASTIK_SRC)/libmastik.a: $(MASTIK_OBJECTS)
	ar rcs $@ $(MASTIK_OBJECTS)

# Compile source files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
# Compile Mastik source files
$(MASTIK_SRC)/%.o: $(MASTIK_SRC)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(MASTIK_OBJECTS) $(MASTIK_SRC)/libmastik.a $(TARGET)

# Force rebuild
rebuild: clean all

# Phony targets
.PHONY: all clean rebuild