#!/bin/bash -e

if [ "$PATH" != "/path" ] ; then
	echo "Bad PATH: $PATH"
	exit 1
fi

if [ "$LD_LIBRARY_PATH" != "/lib" ] ; then
	echo "Bad LD_LIBRARY_PATH: $LD_LIBRARY_PATH"
	exit 1
fi

if [ "$QML2_IMPORT_PATH" != "/bar/qml/import" ] ; then
	echo "Bad QML import path: $QML2_IMPORT_PATH"
	exit 1
fi

exit 0
