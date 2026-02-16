# This sets the output artifact name
EE_BIN = game.elf
# These are the generated objects by the compiler
EE_OBJS = main.o cJSON.o sqlite3.o

# "Virtual path"
VPATH = include
# These are like including dependencies
EE_INCS += -Iinclude -I$(GSKIT)/include -I$(PS2SDK)/ports/include
# These are the linker search paths
EE_LDFLAGS += -L$(GSKIT)/lib -L$(PS2SDK)/ports/lib
# There are like declaring the dependecies
EE_LIBS += -lgskit_toolkit -lgskit -ldmakit -lpad -lpng -lz

# SQLite compile flags for PS2 (in-memory only, no threads, no file I/O)
sqlite3.o: EE_CFLAGS += -DSQLITE_THREADSAFE=0 \
	-DSQLITE_OMIT_LOAD_EXTENSION=1 \
	-DSQLITE_OMIT_WAL=1 \
	-DSQLITE_TEMP_STORE=3 \
	-DSQLITE_OMIT_AUTHORIZATION \
	-DSQLITE_OMIT_DEPRECATED \
	-DSQLITE_OMIT_PROGRESS_CALLBACK \
	-DSQLITE_OMIT_TRACE \
	-w

# This is like a "build" task
all: $(EE_BIN)

# This is like a "clean" task
clean:
	rm -f $(EE_BIN) $(EE_OBJS)

# These are like Gradle plugins
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
