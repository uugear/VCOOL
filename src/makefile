all: debpkg

debpkg: vcool
	cp vcool debpkg/usr/bin/vcool
	cp vcool.stg debpkg/etc/vcool/vcool.stg
	cp init.sh debpkg/etc/init.d/vcool
	chmod 755 debpkg/DEBIAN/postinst
	dpkg --build debpkg "vcool_arm64.deb"

vcool: vcool.c
	gcc -o vcool vcool.c -lgpiod -lpthread

clean:
	rm -f *.deb
	rm -f debpkg/usr/bin/vcool
	rm -f debpkg/etc/vcool/vcool.stg
	rm -f debpkg/etc/init.d/vcool
	rm -f vcool
