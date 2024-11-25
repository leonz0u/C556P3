#include "RoutingProtocolImpl.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <set>

#define DEBUG 1
#define DEBUG_PRINT(fmt, ...) \
    do { if (DEBUG) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)

RoutingProtocolImpl::RoutingProtocolImpl(Node *n) : RoutingProtocol(n) {
    sys = n;
}

RoutingProtocolImpl::~RoutingProtocolImpl() {
}

void RoutingProtocolImpl::init(unsigned short num_ports, unsigned short router_id, 
                              eProtocolType protocol_type) {
    this->num_ports = num_ports;
    this->router_id = router_id;
    this->protocol_type = protocol_type;
    ports.resize(num_ports);
    
    // Set a periodic check timer (check every second)
    AlarmType *alarm_data = new AlarmType(ALARM_PERIODIC_CHECK);
    sys->set_alarm(this, 1000, (void *)alarm_data);
    
    // Initialize the route to itself in the DV table
    DVEntry self_entry;
    self_entry.cost = 0;
    self_entry.next_hop = router_id;
    self_entry.port = INFINITY_COST; // means local
    self_entry.last_updated = sys->time();
    dv_table[router_id] = self_entry;

    // Immediately send the first round of PINGs
    for (unsigned short port = 0; port < num_ports; port++) {
        send_ping(port);
        // Set up subsequent PING timers
        AlarmType *ping_alarm = new AlarmType(ALARM_PING);
        unsigned short *port_data = new unsigned short(port);
        // combine port number and timer type
        void *data = new char[sizeof(AlarmType) + sizeof(unsigned short)];
        memcpy(data, ping_alarm, sizeof(AlarmType));
        memcpy((char*)data + sizeof(AlarmType), port_data, sizeof(unsigned short));
        sys->set_alarm(this, 10000, data);
        delete ping_alarm;
        delete port_data;
    }

    // send first dv update immediately
    if (protocol_type == P_DV) {
        send_dv_update(false);
        // set periodic DV update timer (30 seconds)
        alarm_data = new AlarmType(ALARM_DV_UPDATE);
        sys->set_alarm(this, 30000, (void *)alarm_data);
    }  else if (protocol_type == P_LS) {
        // send first LS update immediately
        send_ls_update(false);

        // set periodic LS update timer (30 seconds)
        AlarmType *ls_update_alarm = new AlarmType(ALARM_LS_UPDATE);
        sys->set_alarm(this, 30000, (void *)ls_update_alarm);
    }
}


void RoutingProtocolImpl::send_ping(unsigned short port) {
    DEBUG_PRINT("Router %d sending PING on port %d\n", router_id, port);
    // Ping Packet Format: | type(1) | reserved(1) | size(2) | src_id(2) | dst_id(2) | payload(4) |
    unsigned short packet_size = 12; // 总大小12字节
    char *packet = new char[packet_size];
    
    // Packet Type: PING
    packet[0] = PING;
    packet[1] = 0; // reserved
    
    unsigned short size_n = htons(packet_size);
    memcpy(packet + 2, &size_n, 2);
    
    unsigned short src_n = htons(router_id);
    memcpy(packet + 4, &src_n, 2);
    
    // broadcast
    unsigned short dst_n = 0;
    memcpy(packet + 6, &dst_n, 2);
    
    unsigned int current_time = sys->time();
    memcpy(packet + 8, &current_time, 4);
    
    // Record the send time
    ports[port].last_ping_time = current_time;

    sys->send(port, packet, packet_size);
    DEBUG_PRINT("Router %d PING sent with timestamp %u\n", router_id, current_time);
}

void RoutingProtocolImpl::handle_ping(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    
    // change the packet type to PONG
    pkt[0] = PONG;
    
    // Swap source and destination addresses
    unsigned short src_id, dst_id;
    memcpy(&src_id, pkt + 4, 2);
    memcpy(&dst_id, pkt + 6, 2);
    
    src_id = ntohs(src_id);
    dst_id = ntohs(dst_id);

    DEBUG_PRINT("Router %d received PING from Router %d on port %d\n", 
                router_id, src_id, port);
    
    // record neighbor id
    if (ports[port].neighbor_id == INFINITY_COST) {
        ports[port].neighbor_id = src_id;
    }
    
    // set up new src and dst id
    unsigned short new_src = htons(router_id);
    unsigned short new_dst = htons(src_id);
    memcpy(pkt + 4, &new_src, 2);
    memcpy(pkt + 6, &new_dst, 2);
    
    sys->send(port, pkt, size);
}

void RoutingProtocolImpl::handle_pong(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    unsigned int current_time = sys->time();
    
    // get the time of send ping
    unsigned int ping_time;
    memcpy(&ping_time, pkt + 8, 4);
    
    unsigned short rtt = current_time - ping_time;
    
    // get and record neighbor id
    unsigned short src_id;
    memcpy(&src_id, pkt + 4, 2);
    src_id = ntohs(src_id);
    
    bool topology_changed = false;
    bool cost_changed = false;
    
    if (!ports[port].is_alive || ports[port].neighbor_id != src_id) {
        topology_changed = true;
    }
    if (ports[port].cost != rtt) {
        cost_changed = true;
    }
    
    // update port status
    ports[port].last_pong_time = current_time;
    ports[port].is_alive = true;
    ports[port].neighbor_id = src_id;
    ports[port].cost = rtt;

    // if using LS protocol, update LS entry last updated time
    if (protocol_type == P_LS) {
        auto link_key = std::make_pair(std::min(router_id, src_id), std::max(router_id, src_id));
        if (ls_database.find(link_key) != ls_database.end()) {
            LSEntry &lsa = ls_database[link_key];
            lsa.last_updated = current_time;
        }
    }
    
    DEBUG_PRINT("Router %d received PONG from Router %d on port %d, RTT = %u ms\n", 
                router_id, src_id, port, rtt);
    
    // if using DV protocol，update route information to neighbor
    if (protocol_type == P_DV) {
        // path cost not rtt because rtt is recorded in port.cost
        bool route_updated = update_dv_entry(src_id, src_id, port, 0);
        
        if (topology_changed || route_updated) {
            send_dv_update(true);
            DEBUG_PRINT("Router %d: Triggered DV update due to neighbor change/update %d\n", 
                       router_id, src_id);
        }
    }

    if (protocol_type == P_LS && (topology_changed || cost_changed)) {
        // update ls_database
        auto link_key = std::make_pair(std::min(router_id, src_id), std::max(router_id, src_id));
        LSEntry &lsa = ls_database[link_key];

        // new lsa or cost changed, seqnumber ++
        if (lsa.seq_num == 0 || lsa.cost != rtt) {
            lsa.seq_num++;
        }

        lsa.src = link_key.first;
        lsa.dst = link_key.second;
        lsa.cost = rtt;
        lsa.last_updated = current_time;

        DEBUG_PRINT("Router %d: Updated LS database for link (%d -> %d) with cost %u and seq_num %u\n",
                    router_id, router_id, src_id, rtt, lsa.seq_num);

        // trigger ls update
        send_ls_update(true);
        compute_shortest_paths();
        DEBUG_PRINT("Router %d: Triggered LS update due to change in link (%d -> %d)\n",
                    router_id, router_id, src_id);
    }

    delete[] pkt;
}

void RoutingProtocolImpl::check_neighbors() {
    unsigned int current_time = sys->time();
    bool topology_changed = false;
    bool ls_changed = false;
    
    for (unsigned short port = 0; port < num_ports; port++) {
        if (ports[port].last_pong_time > 0) {  // if port ever received a PONG
            unsigned int time_since_last_pong = current_time - ports[port].last_pong_time;
            
            // more than 15 seconds since last pong, mark neighbor as dead
            if (time_since_last_pong >= 15000) {
                if (ports[port].is_alive) {
                    ports[port].is_alive = false;
                    topology_changed = true;
                    
                    // record dead neighbor id
                    unsigned short failed_neighbor = ports[port].neighbor_id;
                    
                     if (protocol_type == P_DV) {
                        bool routes_updated = false;
                        for (auto &entry : dv_table) {
                            if (entry.second.next_hop == failed_neighbor) {
                                entry.second.cost = INFINITY_COST;
                                entry.second.last_updated = 0;
                                routes_updated = true;
                            }
                        }
                        if (routes_updated) {
                            topology_changed = true;
                        }
                    }

                    // If using LS, update LS entry cost and last_updated time
                    if (protocol_type == P_LS) {
                        auto link_key = std::make_pair(std::min(router_id, failed_neighbor), std::max(router_id, failed_neighbor));
                        if (ls_database.find(link_key) != ls_database.end()) {
                            LSEntry &lsa = ls_database[link_key];
                            lsa.seq_num += 1;
                            lsa.cost = INFINITY_COST;
                            lsa.last_updated = 0;
                            ls_changed = true;
                            DEBUG_PRINT("Router %d: Removed link (%d -> %d) from LS database\n", 
                                        router_id, router_id, failed_neighbor);
                        }
                    }
                    
                    DEBUG_PRINT("Router %d detected link failure on port %d to Router %d\n", 
                              router_id, port, failed_neighbor);
                }
            }
        }
    }
    
    
    if (topology_changed && protocol_type == P_DV) {
        send_dv_update(true);
    }
    else if (ls_changed && protocol_type == P_LS) {
            send_ls_update(true);
            compute_shortest_paths();
    }
}


void RoutingProtocolImpl::handle_alarm(void *data) {
    AlarmType alarm_type;
    memcpy(&alarm_type, data, sizeof(AlarmType));
    
    switch (alarm_type) {
        case ALARM_PING: {
            // get port number
            unsigned short port;
            memcpy(&port, (char*)data + sizeof(AlarmType), sizeof(unsigned short));
            
            send_ping(port);
            
            // set next alarm
            void *new_data = new char[sizeof(AlarmType) + sizeof(unsigned short)];
            memcpy(new_data, data, sizeof(AlarmType) + sizeof(unsigned short));
            sys->set_alarm(this, 10000, new_data);
            break;
        }
        
        case ALARM_DV_UPDATE: {
            if (protocol_type == P_DV) {
                DEBUG_PRINT("Router %d: Sending periodic DV update at time %u\n", 
                           router_id, sys->time());
                send_dv_update(false);
                
                // set next 30s periodic dv update
                AlarmType *new_alarm = new AlarmType(ALARM_DV_UPDATE);
                sys->set_alarm(this, 30000, (void *)new_alarm);
            }
            break;
        }

        case ALARM_LS_UPDATE:
            if (protocol_type == P_LS) {
                DEBUG_PRINT("Router %d: Sending periodic LS update at time %u\n", 
                           router_id, sys->time());
                // add neighbor SeqNum for valid entries
                add_neighbor_SeqNum();
                send_ls_update(false);
                AlarmType *new_alarm = new AlarmType(ALARM_LS_UPDATE);
                sys->set_alarm(this, 30000, (void *)new_alarm);
            }
            break;
        
        case ALARM_PERIODIC_CHECK: {
            check_neighbors(); 
            
            if (protocol_type == P_DV) {
                check_dv(); 
            } else if(protocol_type == P_LS){
                check_ls();
            }
            
            AlarmType *new_alarm = new AlarmType(ALARM_PERIODIC_CHECK);
            sys->set_alarm(this, 1000, (void *)new_alarm);
            break;
        }
    }
    
    delete[] (char*)data;
}

void RoutingProtocolImpl::recv(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    unsigned char packet_type = pkt[0];
    
    switch (packet_type) {
        case PING:
            handle_ping(port, packet, size);
            break;
        case PONG:
            handle_pong(port, packet, size);
            break;
        case DV:
            if (protocol_type == P_DV) {
                handle_dv_packet(port, packet, size);
            } else {
                delete[] pkt;
            }
            break;
        case LS:
            if (protocol_type == P_LS) {
                handle_ls_packet(port, packet, size);
            } else {
                delete[] pkt;
            }
            break;
        case DATA:
            if (port == SPECIAL_PORT) {
                // Locally generated data packet
                if (protocol_type == P_DV) {
                    forward_dv_data_packet(port, packet, size);
                } else if(protocol_type == P_LS) {
                    forward_ls_data_packet(port, packet, size);
                }
            } else {
                // Received a packet to be forwarded
                unsigned short dst_id;
                memcpy(&dst_id, pkt + 6, 2);
                dst_id = ntohs(dst_id);
                
                if (dst_id == router_id) {
                    // already arrived at destination
                    delete[] pkt;
                } else {
                    // needs forwarding
                    if (protocol_type == P_DV) {
                        forward_dv_data_packet(port, packet, size);
                    } else if(protocol_type == P_LS) {
                        forward_ls_data_packet(port, packet, size);
                    }
                }
            }
            break;
        default:
            delete[] pkt;
            break;
    }
}


void RoutingProtocolImpl::send_dv_update(bool triggered) {
    DEBUG_PRINT("Router %d: Preparing DV update at time %u (triggered=%d)\n", 
                router_id, sys->time(), triggered);
    
    // DV packet head: type(1) + reserved(1) + size(2) + src_id(2) + dst_id(2)
    unsigned short base_size = 8;
    // DV table entry: dest_id(2) + cost(2)
    unsigned short entry_size = 4;

    for (unsigned short port = 0; port < num_ports; port++) {
        // Check if the port has already received a PONG (indicating a neighbor is present)
        if (ports[port].neighbor_id == INFINITY_COST) {
            continue;  // skip port which has no neighbor
        }
        
        std::vector<std::pair<unsigned short, unsigned short>> entries;
        
    // Add all routing entries, including the route to itself
        for (const auto &entry : dv_table) {
            unsigned short cost;
            if (entry.second.port == port) {
                // poison reverse
                cost = INFINITY_COST;
            } else {
                cost = entry.second.cost;
            }
            
            entries.push_back({entry.first, cost});
        }
        
        // Send updates even if there are no entries (at least include the route to itself)
        unsigned short packet_size = base_size + entry_size * entries.size();
        char *packet = new char[packet_size];
        memset(packet, 0, packet_size);  // clear the entire packet
        
        // set the packet head
        packet[0] = DV;
        packet[1] = 0; // reserved
        unsigned short size_n = htons(packet_size);
        memcpy(packet + 2, &size_n, 2);
        unsigned short src_n = htons(router_id);
        memcpy(packet + 4, &src_n, 2);
        unsigned short dst_n = htons(ports[port].neighbor_id);
        memcpy(packet + 6, &dst_n, 2);
        
        // set dv entries
        int offset = base_size;
        for (const auto &entry : entries) {
            unsigned short dest_n = htons(entry.first);
            unsigned short cost_n = htons(entry.second);
            memcpy(packet + offset, &dest_n, 2);
            memcpy(packet + offset + 2, &cost_n, 2);
            offset += entry_size;
        }
        
        DEBUG_PRINT("Router %d: Sending DV update to neighbor %d on port %d with %zu entries\n",
                   router_id, ports[port].neighbor_id, port, entries.size());
        
        sys->send(port, packet, packet_size);
    }
}

// add neighbor SeqNum
void RoutingProtocolImpl::add_neighbor_SeqNum() {
    // get current time
    unsigned int current_time = sys->time();
    // iterlate ls database
    for (auto it = ls_database.begin(); it != ls_database.end(); ) {
        LSEntry &lsa = it->second;
        // if lsa cost is INFINITY_COST, delete it
        if (lsa.cost == INFINITY_COST) {
            it = ls_database.erase(it);
            continue;
        }

        // if src or dst is router_id, increase seq_num
        // only increase seq_num for valid entries (last_updated time - current time < 45s)
        if (lsa.src == router_id || lsa.dst == router_id) {
            if (current_time - lsa.last_updated < 45000) {
                lsa.seq_num++;
            }
        }
        it++;
    }
}


void RoutingProtocolImpl::send_ls_update(bool triggered) {
    DEBUG_PRINT("Router %d: Preparing LS update at time %u (triggered=%d)\n",
                router_id, sys->time(), triggered);

    // LS packet head: type(1) + reserved(1) + size(2) + src_id(2)
    unsigned short base_size = 8;
    // LSA entry: src_id(2) + dst_id(2) + cost(2) + seq_num(4)
    unsigned short entry_size = 10;

    std::vector<LSEntry> lsas;

    for (const auto &entry_pair : ls_database) {
        const LSEntry &lsa = entry_pair.second;

        // only send LSA with src or dst is router_id
        if (lsa.src == router_id || lsa.dst == router_id) {
            lsas.push_back(lsa);
        }
    }

    // Exit if there are no LSAs to avoid sending an empty packet
    if (lsas.empty()) {
        DEBUG_PRINT("Router %d: No LSAs to send, aborting LS update.\n", router_id);
        return;
    }

    // Construct and send an LS update packet
    unsigned short packet_size = base_size + entry_size * lsas.size();
    char *packet = new char[packet_size];
    memset(packet, 0, packet_size); 

    // packet head
    packet[0] = LS;  
    packet[1] = 0; 
    unsigned short size_n = htons(packet_size);
    memcpy(packet + 2, &size_n, 2);
    unsigned short src_n = htons(router_id);
    memcpy(packet + 4, &src_n, 2);

    // add LSA
    int offset = base_size;
    for (const auto &lsa : lsas) {
        unsigned short src = htons(lsa.src);
        unsigned short dst = htons(lsa.dst);
        unsigned short cost = htons(lsa.cost);
        unsigned int seq_num = htonl(lsa.seq_num);

        memcpy(packet + offset, &src, 2);
        memcpy(packet + offset + 2, &dst, 2);
        memcpy(packet + offset + 4, &cost, 2);
        memcpy(packet + offset + 6, &seq_num, 4);

        offset += entry_size;
    }

    // send to all neibors
    for (unsigned short port = 0; port < num_ports; port++) {
        if (!ports[port].is_alive) {
            continue; 
        }

        // Create a copy of the packet for sending
        char *packet_copy = new char[packet_size];
        memcpy(packet_copy, packet, packet_size);
        sys->send(port, packet_copy, packet_size);

        DEBUG_PRINT("Router %d: Sent LS update to neighbor %d on port %d\n",
                    router_id, ports[port].neighbor_id, port);
    }

    delete[] packet;
}


void RoutingProtocolImpl::handle_dv_packet(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    unsigned short src_id;
    memcpy(&src_id, pkt + 4, 2);
    src_id = ntohs(src_id);
    
    DEBUG_PRINT("Router %d: Received DV update from Router %d on port %d\n", 
                router_id, src_id, port);
    
    // ensure it is the update from correct neighbor
    if (ports[port].neighbor_id != src_id) {
        DEBUG_PRINT("Router %d: Ignoring DV update from unexpected neighbor %d on port %d\n",
                   router_id, src_id, port);
        delete[] pkt;
        return;
    }
    
    bool updated = false;
    
    // get dv entry from the packet
    int offset = 8;
    while (offset < size) {
        unsigned short dest, cost;
        memcpy(&dest, pkt + offset, 2);
        memcpy(&cost, pkt + offset + 2, 2);
        dest = ntohs(dest);
        cost = ntohs(cost);
        
        // do not process the update routing to itself
        if (dest == router_id) {
            offset += 4;
            continue;
        }
        
        updated |= update_dv_entry(dest, src_id, port, cost);
        
        offset += 4;
    }
    
    if (updated) {
        DEBUG_PRINT("Router %d: Triggering DV update due to route changes\n", router_id);
        send_dv_update(true);
    }
    
    delete[] pkt;
}


void RoutingProtocolImpl::handle_ls_packet(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    unsigned short src_id;
    memcpy(&src_id, pkt + 4, 2);
    src_id = ntohs(src_id);

    // Ensure this is not from it self
    if (src_id == router_id) {
        DEBUG_PRINT("Router %d: Ignoring LS update from itself on port %d\n",
                    router_id, port);
        delete[] pkt;
        return;
    }

    DEBUG_PRINT("Router %d: Received LS update from Router %d on port %d\n", router_id, src_id, port);

    bool updated = false;    
    unsigned int current_time = sys->time();

    // Extract LSAs from the packet
    int offset = 8;
    while (offset + 10 <= size) {  // Ensure we don't read beyond packet size
        unsigned short lsa_src, lsa_dst, lsa_cost;
        unsigned int seq_num;

        memcpy(&lsa_src, pkt + offset, 2);
        memcpy(&lsa_dst, pkt + offset + 2, 2);
        memcpy(&lsa_cost, pkt + offset + 4, 2);
        memcpy(&seq_num, pkt + offset + 6, 4);

        lsa_src = ntohs(lsa_src);
        lsa_dst = ntohs(lsa_dst);
        lsa_cost = ntohs(lsa_cost);
        seq_num = ntohl(seq_num);

        // Determine if we need to update the LS database
        auto link_key = std::make_pair(std::min(lsa_src, lsa_dst), std::max(lsa_src, lsa_dst));
        bool need_update = false;

        auto it = ls_database.find(link_key);
        if (it == ls_database.end()) {
            // there is no lsa in the database
            need_update = true;
        } else {
            LSEntry &existing_lsa = it->second;

            if (seq_num > existing_lsa.seq_num) {
                need_update = true;

            } else if (seq_num == existing_lsa.seq_num && lsa_cost != existing_lsa.cost) {
                need_update = true;
            } else {
                // seq number is not larger or cost not changed
                need_update = false;
                // but need to update last_updated time
                existing_lsa.last_updated = current_time;
            }
        }

        if (need_update) {
            // Update the link state entry in LSDB
            LSEntry& ls_entry = ls_database[link_key];
            ls_entry.src = link_key.first;
            ls_entry.dst = link_key.second;
            ls_entry.cost = lsa_cost;
            ls_entry.seq_num = seq_num;
            ls_entry.last_updated = current_time;
            updated = true;
        }

        offset += 10; // Each LSA is 10 bytes
    }

    // If an update occurred that changed costs, recompute shortest paths
    if (updated)
    {
        DEBUG_PRINT("Router %d: Triggering shortest path computation due to LS update\n", router_id);
        // Implement flooding mechanism
        for (unsigned short p = 0; p < num_ports; p++)
        {
            if (p == port)
            {
                continue; // Do not flood back to sender
            }
            if (!ports[p].is_alive)
            {
                continue; // Skip inactive ports
            }

            // not send to packet where it originally came from
            if (ports[p].neighbor_id == src_id)
            {
                continue;
            }

            // Send a copy of the packet
            char *pkt_copy = new char[size];
            memcpy(pkt_copy, pkt, size);
            sys->send(p, pkt_copy, size);

            DEBUG_PRINT("Router %d: Flooded LS update to neighbor %d on port %d\n",
                        router_id, ports[p].neighbor_id, p);
        }
        compute_shortest_paths();
    }

    delete[] pkt;
}

void RoutingProtocolImpl::compute_shortest_paths() {
    // Construct node set and initialize distances
    std::set<unsigned short> nodes;
    for (const auto& entry : ls_database) {
        nodes.insert(entry.second.src);
        nodes.insert(entry.second.dst);
    }

    std::map<unsigned short, unsigned int> dist;
    std::map<unsigned short, unsigned short> prev;
    for (auto node : nodes) {
        dist[node] = UINT_MAX; // Infinity
        prev[node] = INFINITY_COST;
    }
    dist[router_id] = 0;

    // Priority queue: (distance, node)
    using NodeDistPair = std::pair<unsigned int, unsigned short>;
    // std::priority_queue<NodeDistPair, std::vector<NodeDistPair>, std::greater<>> pq;
    std::priority_queue<NodeDistPair, std::vector<NodeDistPair>, std::greater<NodeDistPair>> pq;


    pq.push({0, router_id});

    // Dijkstra's algorithm using priority queue
    while (!pq.empty()) {
        // auto [min_dist, min_node] = pq.top();
        NodeDistPair top = pq.top();
        unsigned int min_dist = top.first;
        unsigned short min_node = top.second;
        pq.pop();

        if (min_dist > dist[min_node]) {
            continue; // Skip outdated entries in the priority queue
        }

        for (const auto& entry_pair : ls_database) {
            const LSEntry& ls_entry = entry_pair.second;
            unsigned int cost = ls_entry.cost;
            unsigned short neighbor = INFINITY_COST;

            if (cost == INFINITY_COST) continue;

            if (ls_entry.src == min_node) {
                neighbor = ls_entry.dst;
            } else if (ls_entry.dst == min_node) {
                neighbor = ls_entry.src;
            } else {
                continue;
            }

            unsigned int alt = dist[min_node] + cost;
            if (alt < dist[neighbor]) {
                dist[neighbor] = alt;
                prev[neighbor] = min_node;
                pq.push({alt, neighbor});
            }
        }
    }

    // Construct the routing table (unchanged from original)
    lsRoutingTable.clear();

    for (auto node : nodes) {
        if (node == router_id || dist[node] == UINT_MAX) {
            continue; // Skip self and unreachable nodes
        }

        unsigned short next_hop = node;
        while (prev[next_hop] != router_id && prev[next_hop] != INFINITY_COST) {
            next_hop = prev[next_hop];
        }

        if (prev[next_hop] == INFINITY_COST) continue;

        unsigned short port = INFINITY_COST;
        for (unsigned short p = 0; p < num_ports; p++) {
            if (ports[p].neighbor_id == next_hop && ports[p].is_alive) {
                port = p;
                break;
            }
        }

        if (port == INFINITY_COST) continue;

        LSRouteEntry route_entry;
        route_entry.next_hop = next_hop;
        route_entry.port = port;
        route_entry.cost = dist[node];
        lsRoutingTable[node] = route_entry;
    }

    print_ls_routing_table();
}



void RoutingProtocolImpl::check_dv() {
    unsigned int current_time = sys->time();
    // bool updated = false;
    std::vector<unsigned short> to_delete;
    
    // mark all timed-out routes as INFINITY_COST
    for (auto &entry : dv_table) {
        if (entry.first != router_id) {  // do not process route to itself
            if (current_time - entry.second.last_updated >= 45000 ||
                entry.second.cost == INFINITY_COST) {               
                to_delete.push_back(entry.first);
            }
        }
    }
    
    for (const auto &dest : to_delete) {
        dv_table.erase(dest);
        DEBUG_PRINT("Router %d: Removed invalid route to %d\n", router_id, dest);
    }

}

void RoutingProtocolImpl::check_ls() {
    unsigned int current_time = sys->time();
    bool updated = false;

    for (auto it = ls_database.begin(); it != ls_database.end();) {
        // if no update for 45 seconds or cost is INFINITY_COST, remove it
        if (current_time - it->second.last_updated > 45000 || it->second.cost == INFINITY_COST) {
            DEBUG_PRINT("Router %d: Removing expired link state (%d -> %d)\n",
                        router_id, it->second.src, it->second.dst);
            it = ls_database.erase(it);
            updated = true;
        } else {
            ++it;
        }
    }

    if (updated) {
        compute_shortest_paths();
        // send_ls_update(true);
    }
}

void RoutingProtocolImpl::forward_dv_data_packet(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    unsigned short dst_id;
    memcpy(&dst_id, pkt + 6, 2);
    dst_id = ntohs(dst_id);
    
    // search the routing table
    auto it = dv_table.find(dst_id);
    if (it != dv_table.end() && it->second.port != INFINITY_COST) {
        // make the copy of packet
        char *pkt_copy = new char[size];
        memcpy(pkt_copy, pkt, size);
        sys->send(it->second.port, pkt_copy, size);
    } else {
        // cannot find the route
        delete[] pkt;
    }
}

void RoutingProtocolImpl::forward_ls_data_packet(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    unsigned short dst_id;
    memcpy(&dst_id, pkt + 6, 2);
    dst_id = ntohs(dst_id);

    // search the routing table
    auto it = lsRoutingTable.find(dst_id);
    if (it != lsRoutingTable.end() && it->second.port != INFINITY_COST) {
        // make the copy of packet
        char *pkt_copy = new char[size];
        memcpy(pkt_copy, pkt, size);
        sys->send(it->second.port, pkt_copy, size);
        delete[] pkt;
    } else {
        delete[] pkt;
    }
}


void RoutingProtocolImpl::print_ls_routing_table() {
    DEBUG_PRINT("\nRouter %d LS Routing Table:\n", router_id);
    DEBUG_PRINT("Destination\tNext Hop\tPort\tCost\n");
    for (const auto &entry : lsRoutingTable) {
        DEBUG_PRINT("%d\t\t%d\t\t%d\t%u\n",
                    entry.first,
                    entry.second.next_hop,
                    entry.second.port,
                    entry.second.cost);
    }
    DEBUG_PRINT("\n");
}


bool RoutingProtocolImpl::update_dv_entry(unsigned short dest, unsigned short next_hop,
                                        unsigned short port, unsigned short path_cost) {
    bool updated = false;
    unsigned int current_time = sys->time();

    // do not update route to self
    if (dest == router_id) {
        updated = false;
        return updated;
    }

    // if port is down, then the route is invalid
    if (!ports[port].is_alive) {
        updated = false;
        return updated;
    }

    // if the path cost is INFINITY_COST, then the cost is INFINITY_COST
    // if the path cost is not INFINITY_COST, then the cost is the sum of the path cost and the cost to the next hop
    unsigned short cost = (path_cost == INFINITY_COST) ? INFINITY_COST : path_cost + ports[port].cost;
    
    auto it = dv_table.find(dest);
    if (it == dv_table.end()) {
        // add new route
        if (cost != INFINITY_COST) {
            DVEntry entry;
            entry.next_hop = next_hop;
            entry.port = port;
            entry.cost = cost;
            entry.last_updated = current_time;
            dv_table[dest] = entry;
            updated = true;
            DEBUG_PRINT("Router %d: Adding new route to %d via %d with cost %d\n",
                       router_id, dest, next_hop, cost);
        }
    } else {
        // if receive a route with cost INFINITY_COST and it->second.cost is not INFINITY_COST and next_hop is the same
        if (cost == INFINITY_COST && it->second.cost != INFINITY_COST && next_hop == it->second.next_hop){
            it->second.cost = INFINITY_COST;
            // set last_updated to 0 to indicate that the route is invalid
            it->second.last_updated = 0;
            updated = true;
            DEBUG_PRINT("Router %d: Removing route to %d\n", router_id, dest);
        }
        // if cost is not INFINITY_COST, then the route is valid
        if (cost < INFINITY_COST){
            // if the route is the same as the current one and the cost is the same, only update the last_updated time
            if (it->second.next_hop == next_hop && cost == it->second.cost)
            {
                // if latest update is more than 15 seconds ago, update the last_updated time
                if (current_time - it->second.last_updated > 15000)
                {
                    it->second.last_updated = current_time;
                    updated = true;
                    DEBUG_PRINT("Router %d: Updating last_updated time for route to %d via %d with cost %d\n",
                                router_id, dest, next_hop, cost);
                }
                updated = false;
                return updated;
            }            
            // check cost if it is better than the current one or it is the same route but with different cost
            if (cost < it->second.cost || (it->second.next_hop == next_hop && cost != it->second.cost))
            {
                it->second.next_hop = next_hop;
                it->second.port = port;
                it->second.cost = cost;
                it->second.last_updated = current_time;
                updated = true;
                DEBUG_PRINT("Router %d: Updating route to %d via %d with cost %d\n",
                            router_id, dest, next_hop, cost);
            }
        }
    }
    
    return updated;
}

void RoutingProtocolImpl::print_dv_table() {
    DEBUG_PRINT("\nRouter %d DV Table:\n", router_id);
    DEBUG_PRINT("D\t\tNH\t\tP\t\tC\t\tLU\n");
    for (const auto &entry : dv_table) {
        DEBUG_PRINT("%d\t\t%d\t\t%d\t\t%d\t\t%u\n",
                   entry.first,
                   entry.second.next_hop,
                   entry.second.port,
                   entry.second.cost,
                   entry.second.last_updated);
    }
    DEBUG_PRINT("\n");
}