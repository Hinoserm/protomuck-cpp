#! /bin/sh
#
# Bash script to apt-get requirements to build
# Scimicat 072813
#

if [ "$(id -u)" != "0" ]; then
   echo "This script must be run as root" 1>&2
   exit 1
fi

# lib32z1-dev for -lz
# libmysqlclient-dev for -lmysqlclient
# libssl-dev for -lssl
# libpcre3-dev libpcre++-dev for pcre

if [ -n "$(command -v apt-get)" ]; then
   apt-get install lib32z1-dev libmysqlclient-dev libpcre3-dev libpcre++-dev
   exit 0
fi

if [ -n "$(command -v yum)" ]; then
   yum install lib32z1-dev libmysqlclient-dev libpcre3-dev libpcre++-dev
fi 
