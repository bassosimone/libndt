# NDT Client Engine

[![GitHub license](https://img.shields.io/github/license/measurement-kit/libndt.svg)](https://raw.githubusercontent.com/measurement-kit/libndt/master/LICENSE) [![Github Releases](https://img.shields.io/github/release/measurement-kit/libndt.svg)](https://github.com/measurement-kit/libndt/releases) [![Build Status](https://img.shields.io/travis/measurement-kit/libndt/master.svg)](https://travis-ci.org/measurement-kit/libndt) [![Coverage Status](https://img.shields.io/coveralls/measurement-kit/libndt/master.svg)](https://coveralls.io/github/measurement-kit/libndt?branch=master) [![Build status](https://img.shields.io/appveyor/ci/bassosimone/libndt/master.svg)](https://ci.appveyor.com/project/bassosimone/libndt/branch/master) [![Documentation](https://codedocs.xyz/measurement-kit/libndt.svg)](https://codedocs.xyz/measurement-kit/libndt/)

`libndt` is a Network-Diagnostic-Tool (NDT) C++11 client engine.

## Synopsis

This example runs a NDT download-only nettest with a nearby server. Make sure
you've downloaded the single include files of [nlohmann/json](
https://github.com/nlohmann/json) >= 3.0.0 and of libndt. Assuming you have
put them in the current directory, you can build a minimal NDT client with:

```C++
#include "json.hpp"  // Nlohmann/json must be included before libndt
#include "libndt.hpp"

int main() {
  libndt::Client client;
  client.run();
}
```

Libndt optionally depends on OpenSSL (for TLS support and in the future for
WebSocket support) and cURL (to autodiscover servers). You can use the following
preprocessor macros to tell libndt that such dependencies are available:

- `LIBNDT_HAVE_OPENSSL`: just define this macro to use OpenSSL (it does not
  matter whether the macro is defined to a true or false value);

- `LIBNDT_HAVE_CURL`: just define to use cURL (likewise).

See [codedocs.xyz/measurement-kit/libndt](
https://codedocs.xyz/measurement-kit/libndt/) for API documentation. See
[libndt-client.cpp](libndt-client.cpp) for a usage example. See
[libndt.hpp](libndt.hpp) for the full API.

## Clone

```
git clone --recursive https://github.com/measurement-kit/libndt
```

## Build and test

The library is a single header library, so it does not need to be built. Yet
it is possible to build a simple client and the tests. We use CMake for this
purpose. When using CMake, it will search for OpenSSL and cURL, and if they
are present, it will enable the corresponding features. CMake also downloads
a copy of nlohmann/json as part of configuring the build.

(To see the exact dependencies required on Debian, please check out the
[Dockerfile](https://github.com/measurement-kit/docker-ci/blob/master/debian/Dockerfile)
used when testing in Travis-CI.)

```
cmake .
cmake --build .
ctest -a --output-on-failure .
./libndt-client  -download  # Run a test
```
