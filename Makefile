VERSION=1.6
CFLAGS=-I Fathom/src -ggdb3 -Ofast -flto -fopenmp -DVERSION=\"$(VERSION)\"
CXXFLAGS=-std=c++17 -Ofast -ggdb3 -flto -I Fathom/src -fopenmp -DVERSION=\"$(VERSION)\"
LDFLAGS=-flto -fwhole-program -fopenmp
OBJS=Micah.o psq.o eval.o search.o utils.o tt.o Fathom/src/tbprobe.o syzygy.o eval_par.o

P_TUNE_FILE=tunings/b0fdfbf40fc115efbf0d302c0892f93a54052894.dat

Micah: $(OBJS)
	$(CXX) $(LDFLAGS) -ggdb3 -o Micah $(OBJS) -pthread

clean:
	rm -f $(OBJS) Micah

winbin: Micah
	rm -rf Micah-$(VERSION)-w64
	mkdir Micah-$(VERSION)-w64
	cp Micah.exe /bin/cygboost_system-1_63.dll /bin/cyggcc_s-seh-1.dll /bin/cygstdc++-6.dll /bin/cygwin1.dll Micah-$(VERSION)-w64
	cp $(P_TUNE_FILE) Micah-$(VERSION)-w64/tune.dat
	zip -9vr Micah-$(VERSION)-win64.zip Micah-$(VERSION)-w64
	ls -lah *zip

install: Micah
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp Micah $(DESTDIR)$(PREFIX)/bin
	mkdir -p $(DESTDIR)$(PREFIX)/share/doc/micah-$(VERSION)
	cp tune.dat $(DESTDIR)$(PREFIX)/share/doc/micah-$(VERSION)

package: clean
	mkdir micah-$(VERSION)
	cp -a libchess Fathom *.cpp *.h debian Makefile micah-$(VERSION)
	cp $(P_TUNE_FILE) micah-$(VERSION)/tune.dat
	rm -f micah-$(VERSION)/.git*
	tar czf micah-$(VERSION).tgz micah-$(VERSION)
	rm -rf micah-$(VERSION)
