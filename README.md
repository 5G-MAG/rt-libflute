# File Delivery over Unidirectional Transport (FLUTE)

## Introduction

This repository holds an implementation of the File Delivery over Unidirectional Transport FLUTE) protocol as defined in IETF RFC 6726. This is a protocol for the unidirectional delivery of files over the Internet, which is particularly suited to multicast networks.

### Specifications

Details are defined in [IETF RFC 6726](https://www.rfc-editor.org/rfc/rfc6726).

### About the implementation

Documentation of the source code can be found at: https://5g-mag.github.io/rt-libflute/

## Downloading
````
cd ~
git clone https://github.com/5G-MAG/rt-libflute.git
````

## Build
````
cd libflute/
mkdir build && cd build
cmake -GNinja ..
ninja
````

## Running
 
When installing libflute, it comes with two demo applications, a receiver and a transmitter. Both applications can be found under ``libflute/build/examples``.

### Step 1: Setting up a Flute receiver
To start the Flute receiver type in

````
./flute-receiver
````

The application will listen at the multicast address 238.1.1.95 by default. Check the help page for additional options (``./flute-receiver --help``).

### Step 2: Setting up a Flute transmitter

To start the Flute transmitter type in

````
./flute-transmitter -r 100000 file
````

For file enter a file that shall be transmitted.
  
The parameter -r provides a data rate limit in kbit/s.

> **Note:** Keep in mind, the rate limit should not be set higher than the network allows, otherwise packet loss can occur (UDP transmission).

### Optional: Using IPSec for secure transmission
If you want to ensure, that transmission between to parties shall be encrypted, you can activate IPSec.
  
Simply use the -k parameter on transmitter and receiver side with a. As IPSec key a AES 256-bit key (so 64 character long) is expected. 

* Starting the receiver with IPSec key: 
````
sudo ./flute-receiver -k fdce8eaf81e3da02fa67e07df975c0111ecfa906561e762e5f3e78dfe106498e
````
As soon as the receiver is starting with -k option, a policy is beeing created that ensures that incoming packets with a specific destination address (can be set with -m) are decrypted with the specified IPSec key. 

You can check the policies with
````
sudo ip xfrm state list
sudo ip xfrm policy list
````

* Starting the transmitter with IPSec key:
````
sudo ./flute-transmitter -r 100000 -k fdce8eaf81e3da02fa67e07df975c0111ecfa906561e762e5f3e78dfe106498e file
````
Outgoing packages with a specific destination address (can be set with -m) will be encrypted with the specified IPSec key.

* Optional: Setting superuser rights

To allow the application to set policy entries without superuser privileges for IPSec, set its capabilities 
accordingly. Alternatively, you can run it with superuser rights (``sudo ...``).
````
sudo setcap 'cap_net_admin=eip' ./flute-transmitter
sudo setcap 'cap_net_admin=eip' ./flute-receiver
````
