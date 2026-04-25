make EXTRA_CFLAGS="-Wno-nonnull" perf
sudo make EXTRA_CFLAGS="-Wno-nonnull" prefix=/usr/local install
sudo ldconfig
sudo hash -r
