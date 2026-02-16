#!/bin/bash
docker run --rm -v /root/gskittest:/src -w /src ps2dev/ps2dev \
  sh -c "apk add --no-cache make gmp mpfr4 mpc1 && \
         make clean && make"

cp game.elf /mnt/c/Users/leona/Documents/PS2-Games
