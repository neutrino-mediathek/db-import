###
###  Makefile mv2mariadb
###

PROG_SOURCES = \
	src/$(PROGNAME).cpp \
	src/common/filehelpers.cpp \
	src/common/helpers.cpp \
	src/common/rapidjsonsax.cpp \
	src/configfile.cpp \
	src/curl.cpp \
	src/lzma_dec.cpp \
	src/serverlist.cpp \
	src/sql.cpp

PROGNAME	 = mv2mariadb
BUILD_DIR	 = build
TMP_OBJS	 = ${PROG_SOURCES:.cpp=.o}
TMP_DEPS	 = ${PROG_SOURCES:.cpp=.d}
PROG_OBJS	 = $(addprefix $(BUILD_DIR)/,$(TMP_OBJS))
PROG_DEPS	 = $(addprefix $(BUILD_DIR)/,$(TMP_DEPS))

## (optional) private definitions for DEBUG, EXTRA_CXXFLAGS etc.
## --------------------------------

-include config.mk

DEBUG			?= 0
ENABLE_SANITIZER	?= 0
QUIET			?= 1
DESTDIR			?= ./INSTALL
EXTRA_CXXFLAGS		?= 
EXTRA_LDFLAGS		?= 
EXTRA_INCLUDES		?= 
EXTRA_LIBS		?= 

CXX			?= g++
LD			?= g++
STRIP			?= strip
STDC++			?= -std=c++11

## --------------------------------

ifneq ($(DEBUG), 1)
ENABLE_SANITIZER	 = 0
endif

INCLUDES	 =
INCLUDES	+= -I/usr/include/mariadb
INCLUDES	+= $(EXTRA_INCLUDES)

CXXFLAGS	 = $(INCLUDES) -pipe -fno-strict-aliasing
ifeq ($(DEBUG), 1)
CXXFLAGS	+= -O0 -g -ggdb3
else
CXXFLAGS	+= -O3
endif
CXXFLAGS	+= $(STDC++)
CXXFLAGS	+= -fmax-errors=10
CXXFLAGS	+= -Wall
CXXFLAGS	+= -Wextra
CXXFLAGS	+= -Wshadow
CXXFLAGS	+= -Warray-bounds
CXXFLAGS	+= -Werror
CXXFLAGS	+= -Werror=format-security
CXXFLAGS	+= -Werror=array-bounds
CXXFLAGS	+= -fexceptions
CXXFLAGS	+= -Wformat
CXXFLAGS	+= -Wformat-security
CXXFLAGS	+= -Wuninitialized
CXXFLAGS	+= -funsigned-char
CXXFLAGS	+= -Wstrict-overflow
CXXFLAGS	+= -Woverloaded-virtual
CXXFLAGS	+= -Wunused
CXXFLAGS	+= -Wunused-value
CXXFLAGS	+= -Wunused-variable
CXXFLAGS	+= -Wunused-function
CXXFLAGS	+= -fno-omit-frame-pointer
CXXFLAGS	+= -fstack-protector-all
CXXFLAGS	+= -fstack-protector-strong
CXXFLAGS	+= -Wno-long-long
CXXFLAGS	+= -Wno-narrowing
CXXFLAGS	+= -Winit-self
CXXFLAGS	+= -Wpedantic

ifeq ($(ENABLE_SANITIZER), 1)
## libasan
CXXFLAGS	+= -fsanitize=address
CXXFLAGS	+= -fsanitize=leak
CXXFLAGS	+= -fsanitize=returns-nonnull-attribute
CXXFLAGS	+= -fsanitize=enum
## libubsan
CXXFLAGS	+= -fsanitize=unreachable
CXXFLAGS	+= -fsanitize=undefined
CXXFLAGS	+= -fsanitize=integer-divide-by-zero
CXXFLAGS	+= -fsanitize=signed-integer-overflow
CXXFLAGS	+= -fsanitize=object-size

CXXFLAGS	+= -DSANITIZER
endif

CXXFLAGS	+= $(EXTRA_CXXFLAGS)

LIBS		 =
ifeq ($(ENABLE_SANITIZER), 1)
LIBS		+= -lasan
LIBS		+= -lubsan
endif
LIBS		+= -lmariadb
LIBS		+= -llzma
LIBS		+= -lcurl
LIBS		+= -lpthread
LIBS		+= -lexpat
LIBS		+= $(EXTRA_LIBS)

LDFLAGS		 = $(LIBS)
LDFLAGS		+= $(EXTRA_LDFLAGS)

all-build: $(BUILD_DIR)/$(PROGNAME)
all-build-strip: all-build strip
ifeq ($(DEBUG), 1)
all: all-build
else
all: all-build-strip
endif

ifeq ($(QUIET), 1)
quiet = @
else
quiet =
endif

build/src/%.o: src/%.cpp
	@if ! test -d $$(dirname $@); then mkdir -p $$(dirname $@); fi;
	@if test "$(quiet)" = "@"; then echo "CXX $< => $@"; fi;
	$(quiet)$(CXX) $(CXXFLAGS) -MT $@ -MD -MP -c -o $@ $<

$(BUILD_DIR)/$(PROGNAME): $(PROG_OBJS)
	@if ! test -d $$(dirname $@); then mkdir -p $$(dirname $@); fi;
	@if test "$(quiet)" = "@"; then echo "CXXLD *.o => $@"; fi;
	$(quiet)$(LD) $(LDFLAGS) $(PROG_OBJS) -o $@

install: all
	@if test "$(DESTDIR)" = ""; then \
		echo -e "\nERROR: No DESTDIR specified.\n"; false;\
	fi
	@if test "$(quiet)" = "@"; then echo -e "\nINSTALL $(PROGNAME) => $(DESTDIR)\n"; fi;
#	$(quiet)rm -fr $(DESTDIR)/dl
	$(quiet)rm -fr $(DESTDIR)/sql
	$(quiet)rm -f $(DESTDIR)/$(PROGNAME)
	$(quiet)install -m 755 -d $(DESTDIR)/dl
	$(quiet)install -m 755 -d $(DESTDIR)/sql
	$(quiet)cp -f src/sql/*  $(DESTDIR)/sql
	$(quiet)install -m 755 -D $(BUILD_DIR)/$(PROGNAME) $(DESTDIR)/$(PROGNAME)

clean:
	rm -rf $(BUILD_DIR)

strip:
	@$(STRIP) $(BUILD_DIR)/$(PROGNAME)

-include $(PROG_DEPS)
