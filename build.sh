#!/bin/bash

\cp freeRaptor/*.c src/
\cp freeRaptor/*.h include/
ls src/*.c | xargs -I{} mv {} {}pp
rm -rf build
mkdir build && cd build
cmake -GNinja ..
ninja
cd ../src
ls ../freeRaptor/*.c | awk -F "/" '{print $3"pp"}' | xargs rm -rf
cd ../include
ls ../freeRaptor/*.h | awk -F "/" '{print $3}' | xargs rm -rf
