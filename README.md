# Table of contents
* Installation guide
* Usage
* Documentation

## Installation guide

Installation of libflute consists of 2 simple steps:
1. Getting the source code
2. Building

### Step 1: Getting the source code

This repository uses git submodules so it must be cloned with the following command

````
git clone --recurse-submodules https://github.com/5G-MAG/rt-libflute.git
````

If you had checked it out before the submodules were added or ran git clone withou the `--recurse-submodules` argument then initialise and update the submodules by running

```
git submodule update --init
```

### Step 2: Building
````
cd rt-libflute/
mkdir build
cd build
cmake ..
cmake --build .
````
(alternatively you can use the `build.sh` script to build in debug mode with raptor enabled)

Build options:

- "Enable Raptor": build with support for the raptor10-based forward error correction. On by default. Disable by passing the `-DENABLE_RAPTOR=OFF` option to cmake

## Usage
 
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

### Optional: Forward Error Correction (FEC) for lossy environment

To use forward error correction to overcome packet loss during transmission add the option `-f 1` to the transmitter. The receiver needs no such option, just make sure that both of them were properly built/rebuilt with raptor enabled in the build options (this is the default).

To simulate packet loss over the loopback interface and test that FEC works, you can run the `setup_packet_loss_on_loopback` script, as root.

Then start a transmission as you usually would. Make sure to use the default ip address, since the script will reroute this to go through the loopback interface.

Finally revert the changes to your loopback interface and routing table with the `reset_loopback_settings` script.

## Documentation

Documentation of the source code can be found at: https://5g-mag.github.io/rt-libflute/

To generate it locally via doxygen run `doxygen` in the project root.
Then to view it open the local file `html/index.html` in a browser
