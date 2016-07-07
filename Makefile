PROGS = pitch_follow

all: $(PROGS)

CC = g++ -std=c++11

LFLAGS = -ljack -laubio -lstk

CFLAGS = -Wall -c \
				-I/usr/local/include/stk

LIBS =

SRCS = pitch_follow.cpp utilities.cpp
OBJS = $(SRCS:.cpp=.o)
HDRS =

.SUFFIXES:

.SUFFIXES: .cpp

%.o : %.cpp
	$(CC) ${CFLAGS} $<

pitch_follow: ${OBJS}
	${CC} -o $@ ${OBJS} ${LFLAGS}

clean:
	rm -f ${OBJS} $(PROGS:%=%.o)
