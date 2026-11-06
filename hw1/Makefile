LLVM_CONFIG ?= ../llvm_build/bin/llvm-config
CXX=`$(LLVM_CONFIG) --bindir`/clang
CXXFLAGS=`$(LLVM_CONFIG) --cppflags` -fPIC -fno-rtti
LDFLAGS=`$(LLVM_CONFIG) --ldflags`
IRFLAGS=-Xclang -disable-O0-optnone -fno-discard-value-names -S -emit-llvm
OPT=`$(LLVM_CONFIG) --bindir`/opt
SOURCE_FILE ?= test1.c
IR_FILE = $(SOURCE_FILE:.c=.ll)
.PHONY: all test run clean 
all: hw1.so test
test: $(IR_FILE)
hw1.so: hw1.cpp
	$(CXX) -shared -o $@ $< $(CXXFLAGS) $(LDFLAGS)
$(IR_FILE): $(SOURCE_FILE)
	$(CXX) $(IRFLAGS) -o $@ $<
run: $(IR_FILE) hw1.so
	$(OPT) -disable-output -load-pass-plugin=./hw1.so -passes=hw1 $<
clean:
	rm -f *.o *.ll *.so