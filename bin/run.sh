#!/bin/sh

docker run -ti --rm -v `pwd`:/analyser analyser sh bin/_run.sh

