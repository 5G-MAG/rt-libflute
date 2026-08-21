<h1 align="center">FLUTE Library</h1>
<p align="center">
  <img src="https://img.shields.io/badge/Status-Under_Development-yellow" alt="Under Development">
  <img src="https://img.shields.io/github/v/tag/5G-MAG/rt-libflute?label=version" alt="Version">
  <img src="https://img.shields.io/badge/License-5G--MAG%20Public%20License%20(v1.0)-blue" alt="License">
</p>

## Introduction

Additional information can be found at: https://5g-mag.github.io/Getting-Started/pages/multimedia-content-delivery/

## Installation guide

Installation of libflute consists of 4 simple steps:

1. Getting the source code
2. Installing the dependencies
3. Build setup
4. Building

### Step 1: Getting the source code

````
cd ~
git clone https://github.com/5G-MAG/rt-libflute.git
````

### Step 2: Installing the dependencies

````
sudo apt install ninja-build libboost-all-dev libspdlog-dev libtinyxml2-dev libconfig++-dev clang-tidy clang g++-12 cmake libssl-dev libnl-3-dev zlib1g-dev
````

### Step 3: Build setup

````
cd rt-libflute/
mkdir build && cd build
cmake -GNinja ..
````

If you want to build the project without the unit tests run the following commands instead:

````
cd rt-libflute/
mkdir build && cd build
cmake -GNinja -DBUILD_TESTING=OFF ..
````

### Step 3: Building

````
ninja
````

## Usage

When installing libflute, it comes with two demo applications, a receiver and a transmitter. Both applications can be
found under ``rt-libflute/build/examples``.

### Step 1: Setting up a Flute receiver

To start the Flute receiver type in

````
cd rt-libflute/build/examples
./flute-receiver
````

The application will listen at the multicast address 238.1.1.95 by default. Check the help page for additional options (
``./flute-receiver --help``).

By default, the FLUTE receiver will store the received files under the same path they were transmitted from (essentially
overwriting the files if you are running the transmitter and the receiver on the same machine). To change the output
directly you can use the `-o` option:

````
./flute-receiver -o /path/to/output/directory
````

### Step 2: Setting up a Flute transmitter

To start the Flute transmitter type in

````
cd rt-libflute/build/examples
./flute-transmitter -r 100000 file
````

For file enter a file that shall be transmitted.

The parameter -r provides a data rate limit in kbit/s.

> **Note:** Keep in mind, the rate limit should not be set higher than the network allows, otherwise packet loss can
> occur (UDP transmission).

### Optional: Using IPSec for secure transmission

If you want to ensure, that transmission between to parties shall be encrypted, you can activate IPSec.

Simply use the -k parameter on transmitter and receiver side with a. As IPSec key a AES 256-bit key (so 64 character
long) is expected.

* Starting the receiver with IPSec key:

````
sudo ./flute-receiver -k fdce8eaf81e3da02fa67e07df975c0111ecfa906561e762e5f3e78dfe106498e
````

As soon as the receiver is starting with -k option, a policy is beeing created that ensures that incoming packets with a
specific destination address (can be set with -m) are decrypted with the specified IPSec key.

You can check the policies with

````
sudo ip xfrm state list
sudo ip xfrm policy list
````

* Starting the transmitter with IPSec key:

````
sudo ./flute-transmitter -r 100000 -k fdce8eaf81e3da02fa67e07df975c0111ecfa906561e762e5f3e78dfe106498e file
````

Outgoing packages with a specific destination address (can be set with -m) will be encrypted with the specified IPSec
key.

* Optional: Setting superuser rights

To allow the application to set policy entries without superuser privileges for IPSec, set its capabilities
accordingly. Alternatively, you can run it with superuser rights (``sudo ...``).

````
sudo setcap 'cap_net_admin=eip' ./flute-transmitter
sudo setcap 'cap_net_admin=eip' ./flute-receiver
````

## Conformance profiles: 3GPP MBMS versus general FLUTE

This library serves two different sets of obligations, and they are not degrees of strictness.
A session correct as general FLUTE can be non-conformant as 3GPP MBMS, because the MBMS Download
Profile forbids the sender things RFC 3926 permits.

Select with the trailing `profile` argument on the `Transmitter` and `FileDeliveryTable`
constructors. **The default is `Profile::Mbms3gpp`**, since that is what the specifications
mandating FLUTE for this project require. Pass `Profile::GeneralFlute` for a non-3GPP session.

```cpp
LibFlute::Transmitter tx(addr, port, tsi, mtu, rate, io);                        // 3GPP, default
LibFlute::Transmitter tx(addr, port, tsi, mtu, rate, io, {}, ns, true, {},
                         LibFlute::Profile::GeneralFlute);                       // plain RFC 3926
```

The profile decides which obligations apply. The FDT namespace, a separate argument, decides
which XML schema is emitted. They are independent.

### What the 3GPP profile adds, over general FLUTE

Every row below is a restriction on the **sender** only. Receive-side parsing is unchanged in all
cases, because TS 26.346 annex L.4 keeps most of these optional-to-support for receivers and
mandatory for two of them, so a receiver that refused them would break against a conformant peer.

| Attribute / element | General FLUTE (RFC 3926) | 3GPP MBMS (TS 26.346 annex L.4) |
|---|---|---|
| `Transfer-Length` | permitted | not carried (clause L.4.4) |
| `Complete` | permitted on FDT-Instance | not used by the sender (clause L.4.3) |
| `FEC-OTI-FEC-Instance-ID` | permitted | not used at either level (clause L.4.2) |
| `Content-Encoding` | any value | absent, or `gzip` only; other values refused (clause L.4.2) |
| `Group` element | permitted | not used (clause L.4.2); this library never emits it |

Behaviour required by RFC 3926 and the ALC/LCT documents beneath it applies in **both** profiles
and is not switchable: the LCT header format, the 20-bit FDT Instance ID and its wraparound, the
mandatory `Expires` attribute on FDT-Instance, and EXT_FTI support on any TOI other than 0.

FLUTE version 2 (RFC 6726) and RaptorQ (RFC 6330) are referenced by neither TS 26.346 nor
TS 26.517 at this baseline and are not part of either profile here. They live on their own
branches.

## Testing

To execute the tests make sure to have built the project with testing enabled (see Step 3: Build setup).

Then run

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

## Documentation

Documentation of the source code can be found at: https://5g-mag.github.io/rt-libflute/
