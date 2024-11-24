#ifndef ROUTINGPROTOCOLIMPL_H
#define ROUTINGPROTOCOLIMPL_H

#include "RoutingProtocol.h"
#include "Node.h"
#include <vector>
#include <map>
#include <cstring>
#include <arpa/inet.h>

// Timer type definition
enum AlarmType {
    ALARM_PING = 0,           // PING every 10 seconds
    ALARM_DV_UPDATE,          // DV update every 30 seconds
    ALARM_LS_UPDATE,          // LS update every 30 seconds
    ALARM_PERIODIC_CHECK      // Status check every 1 second (combining neighbor and route checks)
};

// Port status structure
struct PortStatus {
    unsigned short neighbor_id;    // Neighbor router ID, initially set to INFINITY_COST
    unsigned int last_ping_time;   // Time of the last PING sent
    unsigned int last_pong_time;   // Time of the last PONG received
    bool is_alive;                 // Whether the port is active
    unsigned short cost;           // Link Cost RTT
    
    PortStatus() : neighbor_id(INFINITY_COST), last_ping_time(0), 
                  last_pong_time(0), is_alive(false), cost(INFINITY_COST) {}
};

struct DVEntry {
    unsigned short next_hop;       // Next-hop router ID
    unsigned short port;           // Outgoing port
    unsigned short cost;           // Total cost to the destination
    unsigned int last_updated;     // Last update time
    
    DVEntry() : next_hop(INFINITY_COST), port(INFINITY_COST), 
                cost(INFINITY_COST), last_updated(0) {}
};

struct LSEntry {
    unsigned short src;
    unsigned short dst;
    unsigned short cost;
    unsigned int seq_num;
    unsigned int last_updated;

    LSEntry() : src(0), dst(0), cost(0), seq_num(0), last_updated(0) {}
};

struct LSRouteEntry {
    unsigned short next_hop;  // Next-hop router ID
    unsigned short port;      // Outgoing port
    unsigned int cost;        // Total cost to the destination

    LSRouteEntry() : next_hop(INFINITY_COST), port(INFINITY_COST), cost(INFINITY_COST) {}
};

typedef std::map<std::pair<unsigned short, unsigned short>, LSEntry> LSDatabase;
typedef std::map<unsigned short, LSRouteEntry> ls_routing_table;

class RoutingProtocolImpl : public RoutingProtocol {
  public:
    RoutingProtocolImpl(Node *n);
    ~RoutingProtocolImpl();

    void init(unsigned short num_ports, unsigned short router_id, eProtocolType protocol_type);
    // As discussed in the assignment document, your RoutingProtocolImpl is
    // first initialized with the total number of ports on the router,
    // the router's ID, and the protocol type (P_DV or P_LS) that
    // should be used. See global.h for definitions of constants P_DV
    // and P_LS.

    void handle_alarm(void *data);
    // As discussed in the assignment document, when an alarm scheduled by your
    // RoutingProtoclImpl fires, your RoutingProtocolImpl's
    // handle_alarm() function will be called, with the original piece
    // of "data" memory supplied to set_alarm() provided. After you
    // handle an alarm, the memory pointed to by "data" is under your
    // ownership and you should free it if appropriate.

    void recv(unsigned short port, void *packet, unsigned short size);
    // When a packet is received, your recv() function will be called
    // with the port number on which the packet arrives from, the
    // pointer to the packet memory, and the size of the packet in
    // bytes. When you receive a packet, the packet memory is under
    // your ownership and you should free it if appropriate. When a
    // DATA packet is created at a router by the simulator, your
    // recv() function will be called for such DATA packet, but with a
    // special port number of SPECIAL_PORT (see global.h) to indicate
    // that the packet is generated locally and not received from 
    // a neighbor router.

 private:
        Node *sys;                         // System interface
        unsigned short router_id;          // Router ID
        unsigned short num_ports;          // Number of ports
        eProtocolType protocol_type;       // Protocol type
        std::vector<PortStatus> ports;     // Port status table
        std::map<unsigned short, DVEntry> dv_table;  // Distance vector table
        LSDatabase ls_database;            // Link-state database
        ls_routing_table lsRoutingTable;  

        // PING/PONG
        void send_ping(unsigned short port);
        void handle_ping(unsigned short port, void *packet, unsigned short size);
        void handle_pong(unsigned short port, void *packet, unsigned short size);
        void check_neighbors();

        // DV
        void send_dv_update(bool triggered = false);
        void handle_dv_packet(unsigned short port, void *packet, unsigned short size);
        bool update_dv_entry(unsigned short dest, unsigned short next_hop, 
                            unsigned short port, unsigned short cost);
        void check_dv();
        void forward_dv_data_packet(unsigned short port, void *packet, unsigned short size);
        void print_dv_table(); 

        // LS
        void add_neighbor_SeqNum();
        void send_ls_update(bool triggered = false);
        void handle_ls_packet(unsigned short port, void *packet, unsigned short size);
        void update_ls_database(const LSEntry &entry);
        void compute_shortest_paths();
        void check_ls_timeouts();
        void forward_ls_data_packet(unsigned short port, void *packet, unsigned short size);
        void print_ls_routing_table();
        void check_ls();

};

#endif

