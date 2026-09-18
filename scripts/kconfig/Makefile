HOSTCC := gcc
CFLAGS := -std=gnu11 -O2 -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -I.

CORE_SRC := confdata.c symbol.c expr.c util.c preprocess.c menu.c
CORE_OBJS := $(CORE_SRC:.c=.o)

CONF_OBJS := conf.o zconf.tab.o zconf.lex.o $(CORE_OBJS)
NCONF_OBJS := nconf.o nconf.gui.o nconf-common.o zconf.tab.o zconf.lex.o $(CORE_OBJS)

.PHONY: all clean

all: conf nconf

zconf.tab.c zconf.tab.h: parser.y
	bison -t -d -o zconf.tab.c parser.y

zconf.lex.c: lexer.l zconf.tab.h
	flex -o $@ lexer.l

%.o: %.c
	$(HOSTCC) $(CFLAGS) -c -o $@ $<

conf: $(CONF_OBJS)
	$(HOSTCC) -o $@ $^

nconf: $(NCONF_OBJS)
	$(HOSTCC) -o $@ $^ -lncursesw -lmenuw -lpanelw

clean:
	rm -f conf nconf *.o zconf.tab.c zconf.tab.h zconf.lex.c
