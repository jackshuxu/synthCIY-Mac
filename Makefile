CXX      = clang++
CXXFLAGS = -std=c++17 -O2 -Wall -Wno-unused-variable
LDFLAGS  = -framework CoreAudio -framework AudioToolbox -framework CoreFoundation

HEADERS  = olcNoiseMaker.h platform_mac.h

all: synth1 synth2 synth3 synth4
lesson1: lesson1.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) lesson1.cpp -o lesson1 $(LDFLAGS)

synth1: main1.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) main1.cpp -o synth1 $(LDFLAGS)

synth2: main2.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) main2.cpp -o synth2 $(LDFLAGS)

synth3: main3a.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) main3a.cpp -o synth3 $(LDFLAGS)

synth4: main4.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) main4.cpp -o synth4 $(LDFLAGS)

clean:
	rm -f synth1 synth2 synth3 synth4
