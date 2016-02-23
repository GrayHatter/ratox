#! /bin/sh
cd ratox
mkdir ../ratox-$(date +%Y%m%d)
cp * ratox-$(date +%Y%m%d)/*
cd ratox-$(date +%Y%m%d)
debuild -us -uc