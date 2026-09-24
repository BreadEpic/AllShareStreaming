# Docker for AllShare (Powered by MMC)

Build and run it with docker compose from this folder:
```sh
docker compose up -d --build
```
The image downloads the latest [AllShare release](https://github.com/BreadEpic/AllShareStreaming/releases/latest), so a release has to be published first (push a `v*` tag, CI builds it).

Or run the image directly:
```sh
docker build -t allshare .
docker run -d -p 8080:8080 -p 40000-40100:40000-40100/udp -e WEBRTC_PORT_RANGE=40000:40100 -e WEBRTC_NAT_1TO1_HOST=YOUR_LAN_IP allshare
```
and replace `YOUR_LAN_IP` with the device ip address of the local network.

`WEBRTC_PORT_RANGE` must match the udp port mapping.

## Wake-on-LAN (waking up a sleeping pc)

Docker's default bridge network doesn't forward broadcast packets to your LAN, so "Wake Up" might not reach the pc.
Either:
- run the container with `--network host` / `network_mode: host` (Linux only, the port mappings aren't needed then), or
- add the broadcast address of your LAN to the config: `"moonlight": { "wake_on_lan_addresses": ["192.168.1.255"] }`

# Running with a TURN server

1. Copy the [docker-compose.with-turn.yaml](./docker-compose.with-turn.yaml) into your own `docker-compose.yaml`.

2. Create a new `.env` file with:
```dotenv
LAN_ADDRESS=127.0.0.1 # Change this to the device ip address of the local network.

TURN_URL=myturn.com
TURN_USERNAME=myrandomuser
TURN_CREDENTIAL=myrandompass
```

3. Run with docker-compose
```sh
docker compose up --build
```
