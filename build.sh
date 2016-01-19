rm -rf build
mkdir build
g++ depthfeeder.cpp libjson.a -std=c++11 -lcurl -lpthread -Wall -Werror -O3 -o build/depthfeeder
