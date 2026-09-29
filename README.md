<p align="center">
  <img src=".github/banner.svg" width="100%" alt="Reference Tools · Content Delivery Protocols: FLUTE Protocol and Profiles">
</p>

<p align="center">
  A C++17 library implementing the FLUTE/ALC protocol (File Delivery over Unidirectional Transport),
  with a demo transmitter and receiver.
</p>

<p align="center">
  <img alt="Status: under development"
    src="https://img.shields.io/badge/Status-Under%20Development-e67e22">
  <a href="https://github.com/5G-MAG/rt-libflute/releases"><img alt="Version"
    src="https://img.shields.io/github/v/release/5G-MAG/rt-libflute?label=Version"></a>
  <a href="LICENSE"><img alt="License: 5G-MAG Public License v1.0"
    src="https://img.shields.io/badge/License-5G--MAG%20PL%20v1.0-blue"></a>
</p>

<p align="center">
  <a href="https://www.5g-mag.com/reference-tools/content-delivery/">Project page</a> &nbsp;&middot;&nbsp;
  <a href="https://github.com/5G-MAG/rt-libflute/issues">Issues</a> &nbsp;&middot;&nbsp;
  <a href="https://www.5g-mag.com/contributing">Contributing</a>
</p>

---

## At a glance

|  |  |
|---|---|
| **Part of** | [Content Delivery Protocols](https://www.5g-mag.com/reference-tools/content-delivery/), alongside [rt-cmmf-encoder](https://github.com/5G-MAG/rt-cmmf-encoder), [rt-mbms-client](https://github.com/5G-MAG/rt-mbms-client), [rt-media-origin](https://github.com/5G-MAG/rt-media-origin) |

## Introduction

rt-libflute is a C++17 library that sends and receives files over IP multicast using FLUTE. It
builds the library, `flute`, and two demo applications, a transmitter and a receiver, which can
optionally protect the transmission with IPsec. Other 5G-MAG repositories use it as a submodule, for
example the `flute-ffmpeg` tool in
[rt-mbms-examples](https://github.com/5G-MAG/rt-mbms-examples).

More information is on the [project page](https://www.5g-mag.com/reference-tools/content-delivery/).

## Install dependencies

````
sudo apt install ninja-build libboost-all-dev libspdlog-dev libtinyxml2-dev libconfig++-dev clang-tidy clang g++-12 cmake libssl-dev libnl-3-dev zlib1g-dev
````

## Downloading

````
cd ~
git clone https://github.com/5G-MAG/rt-libflute.git
````

## Building

### Build setup

````
cd rt-libflute/
mkdir build && cd build
cmake -GNinja ..
````

To build the project without the unit tests, run these commands instead:

````
cd rt-libflute/
mkdir build && cd build
cmake -GNinja -DBUILD_TESTING=OFF ..
````

### Build

````
ninja
````

## Running

The build produces two demo applications, a receiver and a transmitter, in
``rt-libflute/build/examples``. The `cd` commands below start from the directory that contains the
clone (your home directory, if you followed [Downloading](#downloading)).

### Starting a FLUTE receiver

To start the FLUTE receiver:

````
cd rt-libflute/build/examples
./flute-receiver
````

By default the receiver listens on the multicast address 238.1.1.95. The help page lists the other
options (``./flute-receiver --help``).

By default, the receiver stores the received files under the same path they were transmitted from,
which overwrites the files if the transmitter and the receiver run on the same machine. To change the
output directory, use the `-o` option:

````
./flute-receiver -o /path/to/output/directory
````

### Starting a FLUTE transmitter

To start the FLUTE transmitter:

````
cd rt-libflute/build/examples
./flute-transmitter -r 100000 file
````

Replace `file` with the file to transmit. The `-r` parameter sets a data rate limit in kbit/s.

> **Note:** do not set the rate limit higher than the network allows, otherwise packets can be lost
> (the transmission is over UDP).

### Optional: using IPsec for secure transmission

To encrypt the transmission between the two parties, enable IPsec: give the same key with the `-k`
parameter to both the transmitter and the receiver. The key is a 256-bit AES key, written as 64
hexadecimal characters.

* Starting the receiver with an IPsec key:

````
sudo ./flute-receiver -k fdce8eaf81e3da02fa67e07df975c0111ecfa906561e762e5f3e78dfe106498e
````

When the receiver starts with `-k`, it creates a policy so that incoming packets for a specific
destination address (set with `-m`) are decrypted with that key.

You can check the policies with:

````
sudo ip xfrm state list
sudo ip xfrm policy list
````

* Starting the transmitter with an IPsec key:

````
sudo ./flute-transmitter -r 100000 -k fdce8eaf81e3da02fa67e07df975c0111ecfa906561e762e5f3e78dfe106498e file
````

Outgoing packets to a specific destination address (set with `-m`) are encrypted with that key.

* Optional: setting superuser rights

To let the applications set IPsec policy entries without superuser privileges, set their
capabilities accordingly. Alternatively, run them with superuser rights (``sudo ...``).

````
sudo setcap 'cap_net_admin=eip' ./flute-transmitter
sudo setcap 'cap_net_admin=eip' ./flute-receiver
````

## Development

### Testing

To run the tests, build the project with testing enabled (the first command set under
[Build setup](#build-setup)). Then, from the repository root, run:

````
cd build/tests
ctest
````

To run only the unit tests:

````
ctest -R '^unit:'
````

To run only the end-to-end FLUTE transmitter/receiver test:

````
ctest -R '^e2e:'
````

To see the end-to-end test's transmit/receive debug output locally:

````
ctest -R '^e2e:' --verbose
````

### Documentation

The source code documentation is at https://5g-mag.github.io/rt-libflute/

## Contributing

Contributions are welcome. How to raise an issue, fork the repository and open a pull request, and
the Contributor License Agreement required before code can be merged, are described at
<https://www.5g-mag.com/contributing>.

## License

Distributed under the 5G-MAG Public License v1.0. See [LICENSE](LICENSE).
