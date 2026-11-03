#!/bin/bash
docker run --rm -v "$(pwd):/src" -w /src expserver-tester-image ./build.sh

