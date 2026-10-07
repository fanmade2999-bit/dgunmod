// netmod_api.h -- shared between netmod_hook.cpp and netmod_udp.cpp
#pragma once
#include <cstdint>

struct RemoteTransform {
    float    x, y, z, angle;
    uint32_t seq;  // sender's sequence number of the packet this came from
};

extern "C" {
void netmod_net_start();                                              // netmod_udp.cpp
void netmod_net_stop();
void netmod_set_peer(const char* dotted_ipv4);                        // unicast / directed broadcast target
void netmod_update_local_transform(float x, float y, float z, float angle);  // game thread, every frame
bool netmod_get_remote_transform(RemoteTransform* out);               // game thread; true if new packet
}
