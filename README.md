# QNX Zombies

QNX Zombies is a first person zombie shooter game with cross platform multiplayer 
support for Linux. It runs using a client-server model, where a given QNX machine 
can act as both an authoritative server and a client, with the option to connect 
to other QNX or Linux clients as well, provided that the clients can reach the 
server's IP over the same network.

The server is responsible for spawning different waves of zombies, and keeps 
track of every entity’s position, health, and damage over a custom UDP protocol 
for every client connected to the server.

We wanted to use the OS as the game engine itself, so we built the QNX client as 
a native 3D client directly using QNX’s Screen API, EGL, and raw OpenGL ES 2.0. 
The wall texture seen in the video attached is a brick pattern generated live in 
a shader, with every letter on screen coming from a custom font rasterized at 
build time.

We also rebuilt this game in Godot to provide Linux support and enable cross 
platform multiplayer between Linux and QNX, with the option to manually change 
the IP address it connects to. This client version also includes support for 
player movement using gesture recognition with OpenCV and MediaPipe, and gameplay 
using a custom hand held controller, although the latter is still a work in progress.

## Prerequisites
 
- Your computer and your QNX machine must be connected to the same network.
- Replace `xx.xx.xx.xx` with that network's actual IP address wherever seen in 
this guide.

## Setup

Create an SSH config directory, if you don't already have one:
```
mkdir -p ~/.ssh && chmod 700 ~/.ssh
```

Add an alias called `qnxpi` to refer to your network's IP address, as is 
required by the game's scripts:
```
cat >> ~/.ssh/config << 'EOF'
Host qnxpi
    HostName xx.xx.xx.xx
    User qnxuser
    ServerAliveInterval 30
    ServerAliveCountMax 3
EOF
```

Clone the repo:
```
git clone git@github.com:arshjameel/qnx_zombies_cuhacking7.git
cd qnx_zombies_cuhacking7
```

Make the helper scripts executable:
```
chmod +x start_server.sh run_qnx_client.sh run_godot_client.sh
```

## Running the QNX client over SSH

When running the game for the first time, the following command will build, 
deploy, and start the server over SSH. 
```
./start_server.sh
```

You'll be prompted for your QNX machine's password a couple of times. On a fresh 
machine this is typically `qnxuser`, unless you've changed it.

In a new terminal, run the following command to start the game to on your QNX machine.
```
./run_qnx_client.sh
```

You'll be prompted for the password a couple more times here again.

## Running the QNX client directly on the QNX machine

Assuming the server and QNX client were ran on your QNX machine via SSH at least 
once, the following commands can be later used to directly start the server and 
run the game on your QNX machine respectively:

```
./game/server_qnx
```

```
./game/qnx_client
```

## Running the Godot client on your local linux computer

To play the game on your own linux computer, open a new terminal and run the 
Godot client:
```
./run_godot_client
```

NOTE: You must start the game server on the QNX machine first, and you must 
specify your network's IP address in the settings menu of the game. 
(Only available in the Godot version).