#Sanitizer builds, shared by the game and library makefiles.
#  make -f source/linux/Makefile compile SANITIZE=thread
#  make -f source/linux/Makefile compile SANITIZE=address
#A sanitizer build is a debug build at -O1, kept in obj_d-<sanitizer> and
#bin_d-<sanitizer> so it never mixes with normal debug objects.
#AngelScript and GLFW are built without instrumentation.

DEBUG_OPT = -O0

ifdef SANITIZE
DEBUG = 1
DEBUG_OPT = -O1
SANDIR = -$(SANITIZE)
CXXFLAGS += -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
ifeq ($(SANITIZE), address)
CXXFLAGS += -fsanitize=undefined
endif
endif
