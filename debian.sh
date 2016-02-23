#! /bin/sh
cd ratox
tar -cJf $(echo ../ratox-$(date +%Y%m%d) | tr "-" "_").orig.tar.xz .
rm -rf $(echo ../ratox-$(date +%Y%m%d) | tr "-" "_")
mkdir $(echo ../ratox-$(date +%Y%m%d) | tr "-" "_")
cp -R * $(echo ../ratox-$(date +%Y%m%d) | tr "-" "_")
cd $(echo ../ratox-$(date +%Y%m%d) | tr "-" "_")
debuild -us -uc
