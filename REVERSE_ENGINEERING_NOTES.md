# DGuns 1.09 Multiplayer Reverse-Engineering Notes

## Baseline verified
- Android package: `jp.shade.DGuns5`
- Native game library: `libapp_lib.so` (ARM64 build used for the current patch)
- Game assets are bundled in `assets/zip/bin.zip`, which contains:
  - `dat.bin` — approximately 0.85 MB
  - `grp.bin` — approximately 31.0 MB
  - `bgm.bin` — approximately 4.52 MB
  - `se.bin` — approximately 0.38 MB
- The packed assets expose mission and area-related strings (including `AREA01`, `Mission Select`, `NEXT MISSION`, `MECHA SELECT`, and boss/area state labels), but printable strings alone do not provide a reliable complete list of selectable maps. Map IDs and asset relationships still need structural parsing and runtime confirmation.

## Confirmed multiplayer prototype
- The injected `libnetmod.so` starts a UDP transport on port 8888.
- The current wire packet is 28 bytes: magic, protocol version, sender ID, sequence number, and position/facing transform.
- Transform snapshots are sent approximately every 33 ms.
- The current game hook targets the native player process table entry and creates a remote proxy mech.
- Proxy movement, turning, and animation mirroring have been observed in device testing.
- This remains a one-remote-proxy diagnostic prototype, not yet a complete multi-player session implementation.

## Transport correction in this revision
- Previous code assumed the hotspot broadcast address was `192.168.43.255`, which is not valid for every Android hotspot or Wi-Fi subnet.
- The updated transport discovers active IPv4 interfaces and sends to each available interface broadcast address by default.
- Explicit peer-IP routing remains supported by `netmod_set_peer`.
- Sampled transmit diagnostics report whether packets were sent through interface broadcast or explicit unicast.

## Known gaps before claiming full multiplayer
1. The UI does not yet provide room creation/joining, player lists, map selection, or connection status.
2. The native prototype currently consumes a single latest remote transform and creates one proxy; it does not yet support an arbitrary player roster.
3. Position packets do not yet include map/stage identity, spawn assignments, team/room IDs, readiness state, or a compatibility check.
4. Combat events (firing, projectile ownership, hits, damage, death/respawn) are not synchronized yet.
5. NPC target eligibility for remote players still needs validation against native team/target filters.
6. Direct online peer-to-peer connectivity across different mobile carriers/routers needs separate NAT-traversal testing. The local hotspot mode must remain independent of internet access.
7. The full selectable-map inventory must be recovered from packed asset structure and confirmed against the in-game mission/map selection flow.

## Recommended test order
1. Build/install the interface-broadcast diagnostic revision on two devices.
2. Put both devices on the same hotspot/Wi-Fi network and inspect `netmod_udp` logs for transmit and receive packets.
3. Confirm proxy presence and stable movement, then test reconnect and stale-packet handling.
4. Parse map/stage IDs and confirm both clients can load the same local map assets.
5. Add room/session state and multi-player slots before implementing combat synchronization.
