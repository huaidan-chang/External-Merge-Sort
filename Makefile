CPPOPT=-g -Og -D_DEBUG
# -O2 -Os -Ofast
# -fprofile-generate -fprofile-use
CPPFLAGS=$(CPPOPT) -Wall -ansi -pedantic -std=c++11
# -Wparentheses -Wno-unused-parameter -Wformat-security
# -fno-rtti -std=c++11 -std=c++98

# documents and scripts
DOCS=Tasks.txt
SCRS=

# headers and code sources
HDRS=	defs.h \
		Iterator.h Scan.h Filter.h Sort.h gen.h Dram.h DataRecord.h Leaf.h PQ.h global.h
SRCS=	defs.cpp Assert.cpp Test.cpp \
		Iterator.cpp Scan.cpp Filter.cpp Sort.cpp gen.cpp Dram.cpp PQ.cpp global.cpp TraceFile.cpp

# compilation targets
OBJS=	defs.o Assert.o Test.o \
		Iterator.o Scan.o Filter.o Sort.o gen.o Dram.o PQ.o global.o TraceFile.o
 
# RCS assists
REV=-q -f
MSG=no message

# default target
#
all: Test.exe verify.exe
Test.exe : Makefile $(OBJS)
	g++ $(CPPFLAGS) -o sort $(OBJS)

verify.exe: verify.cpp
	g++ $(CPPFLAGS) -o $@ $< -lz

trace : Test.exe Makefile
	@date > trace
	./Test.exe >> trace
	@size -t Test.exe $(OBJS) | sort -r >> trace

$(OBJS) : Makefile defs.h
Test.o : Iterator.h Scan.h Filter.h Sort.h gen.h SortState.h Dram.h
Iterator.o Scan.o Filter.o Sort.o : Iterator.h
Scan.o : Scan.h
Filter.o : Filter.h
Sort.o : Sort.h
gen.o: gen.h
Dram.o: Dram.h
PQ.o: PQ.h Leaf.h
global.o: global.h
TraceFile.o : TraceFile.h SortState.h

list : Makefile
	echo Makefile $(HDRS) $(SRCS) $(DOCS) $(SCRS) > list
count : list
	@wc `cat list`

ci :
	ci $(REV) -m"$(MSG)" $(HDRS) $(SRCS) $(DOCS) $(SCRS)
	ci -l $(REV) -m"$(MSG)" Makefile
co :
	co $(REV) -l $(HDRS) $(SRCS) $(DOCS) $(SCRS)

clean :
	@rm -f $(OBJS) Test.exe Test.exe.stackdump trace
