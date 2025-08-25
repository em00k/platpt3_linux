#!/bin/bash

if [ "$1" == "clean" ]; then
    rm playpt3
    echo "Cleaned"
    exit 0
fi

gcc -o playpt3 playpt3.c pt3player.c ayumi.c load_text.c -lasound -lm

if [ $? -ne 0 ]; then
    echo "Build failed"
    exit 1
fi

echo "Build complete"

if [ "$1" == "run" ]; then
    ./playpt3 TESKO.PT3
fi