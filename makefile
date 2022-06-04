bin:=$(notdir $(shell pwd))
src:=$(shell ls *.cc)
obj:=$(src:.cc=.o)
lib:=gbm egl glesv2

CXXFLAGS+=$(shell pkg-config --cflags $(lib))
LDFLAGS+=$(shell pkg-config --libs $(lib))

all: $(bin)

$(bin): $(obj)
	$(CXX) $^ $(LDFLAGS) -o $@

%.o: %.cc *.h
	$(CXX) -c $< $(CXXFLAGS) -o $@

clean:
	-rm $(bin) $(obj)

.PHONY: all clean
