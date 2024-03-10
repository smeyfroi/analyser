#!/bin/sh

docker run -ti --rm \
  -v `pwd`:/analyser \
  -p 8000:8000/udp \
  analyser \
  sh bin/_run.sh
