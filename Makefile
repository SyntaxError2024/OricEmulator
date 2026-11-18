CC      := gcc
WINDRES := windres

CFLAGS  := -O2 -UUNICODE -U_UNICODE
LDFLAGS := -mwindows -s
LDLIBS  := -lgdi32 -lwinmm -lshell32 -luser32

TARGET  := Oric.exe
OBJDIR  := obj

SOURCES := $(wildcard src/*.c)
OBJECTS := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SOURCES))
RESOBJ  := $(OBJDIR)/res.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	-taskkill /f /im $(TARGET) >NUL 2>&1
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: src/%.c
	-@if not exist "$(OBJDIR)" mkdir "$(OBJDIR)"
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	-@if exist "$(OBJDIR)" rmdir /S /Q "$(OBJDIR)"
