#!/bin/bash

set -e

#prereqs
sudo apt-get install geotiff-bin libgeotiff-dev libgeotiff2
if [ ! -e /usr/lib/libproj.so ]; then
	sudo ln -s /usr/lib/libproj.so.0 /usr/lib/libproj.so
fi

#get the src
curl -O http://download.osgeo.org/gdal/2.0.0/gdal-2.0.0.tar.gz

#install it
tar pxvf gdal-2.0.0.tar.gz
pushd gdal-2.0.0
./autogen.sh
./configure
make -j6
sudo make install
