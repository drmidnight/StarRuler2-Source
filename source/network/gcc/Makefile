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
BIN = libnetwork.a
SRCDIR = source/network/source

ifeq ($(UNAME), Darwin)
	OSNAME = osx
else
	OSNAME = lin
endif

ifdef DEBUG
	OBJDIR = obj_d$(SANDIR)/$(OSNAME)$(ARCH)/network
	BINDIR = obj_d$(SANDIR)/$(OSNAME)$(ARCH)

	CXXFLAGS += $(DEBUG_OPT) -g
	CXXFLAGS += -D_DEBUG
else
	OBJDIR = obj/$(OSNAME)$(ARCH)/network
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

CXXFLAGS += -std=c++20
CXXFLAGS += -Wall -Wno-invalid-offsetof -Wno-switch -Wno-reorder
CXXFLAGS += -I./source/network/include
CXXFLAGS += -I./source/os/include

#Source code files
SOURCES = \
	address.cpp \
	message.cpp \
	time.cpp \
	init.cpp \
	transport.cpp \
	message_handler.cpp \
	connection.cpp \
	sequence.cpp \
	server.cpp \
	client.cpp \
	lobby.cpp

CPP_FILES = $(addprefix $(SRCDIR)/, $(SOURCES))
OBJ_FILES = $(addprefix $(OBJDIR)/, $(SOURCES:.cpp=.o))
DEP_FILES = $(addprefix $(OBJDIR)/, $(SOURCES:.cpp=.d))

-include $(DEP_FILES)

compile: $(DEP_FILES) $(BINDIR)/$(BIN)

#Dependency files to take care of header changes
$(OBJDIR)/%.d: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	@$(CC) $(CXXFLAGS) -MM -MT "$(@:.d=.o)" $< >> $@

#Object files are compiled separately
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
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
