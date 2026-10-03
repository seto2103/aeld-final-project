#!/bin/bash
#Script to remove all buildroot build artifacts and configuration

cd `dirname $0`
make -C buildroot distclean
