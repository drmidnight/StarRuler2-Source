include source/linux/sanitize.mk

#Find the correct architecture to build for
ifndef ARCH
	ARCH = $(shell getconf LONG_BIT)
endif

#Set correct flags for architecture
ifeq ($(ARCH), 32)
	#Global
	CXXFLAGS += -m32 -march=pentium4 -mtune=generic

	#Arch name
	ARCHNAME = x86
else
	#Global

	#Arch name
	ARCHNAME = x64
endif

#Global flags
UNAME = $(shell uname)
MACHINE = $(shell uname -m)
ifeq ($(origin AR), default)
ifeq ($(UNAME), Darwin)
	AR = ar
else
	AR = gcc-ar
endif
endif
BIN = libircclient.a
SRCDIR = source/libircclient/src

ifeq ($(UNAME), Darwin)
	OSNAME = osx
else
	OSNAME = lin
endif

ifdef DEBUG
	OBJDIR = obj_d$(SANDIR)/$(OSNAME)$(ARCH)/libircclient
	BINDIR = obj_d$(SANDIR)/$(OSNAME)$(ARCH)

	CXXFLAGS += $(DEBUG_OPT) -g
	CXXFLAGS += -D_DEBUG
else
	OBJDIR = obj/$(OSNAME)$(ARCH)/libircclient
	BINDIR = obj/$(OSNAME)$(ARCH)

ifeq ($(UNAME), Darwin)
	CXXFLAGS += -O3 -ffast-math
else
	CXXFLAGS += -Ofast
endif
	CXXFLAGS += -DNDEBUG
ifndef NLTO
	CXXFLAGS += -flto
else
	CXXFLAGS += -fno-lto
endif
endif

ifndef CC
	CC = g++
endif

CXXFLAGS += -I./source/libircclient/include -I./source/libircclient/source
CXXFLAGS += -DENABLE_THREADS -DHAVE_LOCALTIME_R

ifneq ($(UNAME), Darwin)
	CXXFLAGS += -DHAVE_GETHOSTBYNAME_R
endif

#Source code files
SOURCES = \
	libircclient.c

C_FILES = $(addprefix $(SRCDIR)/, $(SOURCES))
OBJ_FILES = $(addprefix $(OBJDIR)/, $(SOURCES:.c=.o))
DEP_FILES = $(addprefix $(OBJDIR)/, $(SOURCES:.c=.d))

-include $(DEP_FILES)

compile: $(DEP_FILES) $(BINDIR)/$(BIN)

#Dependency files to take care of header changes
$(OBJDIR)/%.d: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CXXFLAGS) -MM -MT "$(@:.d=.o)" $< >> $@

#Object files are compiled separately
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo $<
	@$(CC) $(CXXFLAGS) $< -c -o $@

#Complete binary compile
$(BINDIR)/$(BIN): $(OBJ_FILES)
	@mkdir -p $(BINDIR)
	$(AR) rcs $@ $^

clean:
	@rm -rf $(OBJDIR)
	@rm -f ./.depend
