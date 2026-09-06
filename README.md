# QNX Zombies

QNX Zombies is a video game submission for the QNX challenge at CuHacking7.

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
cd game/server_qnx
```

```
cd game/qnx_client
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