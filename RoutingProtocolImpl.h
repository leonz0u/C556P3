#ifndef ROUTINGPROTOCOLIMPL_H
#define ROUTINGPROTOCOLIMPL_H

#include "RoutingProtocol.h"
#include "Node.h"
#include <vector>
#include <map>
#include <cstring>
#include <arpa/inet.h>

// 定时器类型定义
enum AlarmType {
    ALARM_PING = 0,           // 10秒一次的PING
    ALARM_DV_UPDATE,          // 30秒一次的DV更新
    ALARM_LS_UPDATE,          // 30秒一次的LS更新
    ALARM_PERIODIC_CHECK      // 1秒一次的状态检查(合并邻居和路由检查)
};

// 端口状态结构
struct PortStatus {
    unsigned short neighbor_id;    // 邻居路由器ID，初始为INFINITY_COST
    unsigned int last_ping_time;   // 上次发送PING的时间
    unsigned int last_pong_time;   // 上次收到PONG的时间
    bool is_alive;                 // 端口是否活跃
    unsigned short cost;           // 链路成本(RTT)
    
    PortStatus() : neighbor_id(INFINITY_COST), last_ping_time(0), 
                  last_pong_time(0), is_alive(false), cost(INFINITY_COST) {}
};

// DV表项结构
struct DVEntry {
    unsigned short next_hop;       // 下一跳路由器ID
    unsigned short port;           // 出口端口
    unsigned short cost;           // 到目的地的总成本
    unsigned int last_updated;     // 最后更新时间
    
    DVEntry() : next_hop(INFINITY_COST), port(INFINITY_COST), 
                cost(INFINITY_COST), last_updated(0) {}
};

// LS表项结构
struct LSEntry {
    unsigned short src;
    unsigned short dst;
    unsigned short cost;
    unsigned int seq_num;
    unsigned int last_updated;

    LSEntry() : src(0), dst(0), cost(0), seq_num(0), last_updated(0) {}
};

struct LSRouteEntry {
    unsigned short next_hop;  // 下一跳路由器 ID
    unsigned short port;      // 出口端口
    unsigned int cost;        // 到目的地的总成本

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
        Node *sys;                         // 系统接口
        unsigned short router_id;          // 路由器ID
        unsigned short num_ports;          // 端口数量
        eProtocolType protocol_type;       // 协议类型
        std::vector<PortStatus> ports;     // 端口状态表
        std::map<unsigned short, DVEntry> dv_table;  // 距离向量表
        LSDatabase ls_database; // 链路状态数据库
        ls_routing_table lsRoutingTable;  // 修改成员变量名称

        // PING/PONG相关方法
        void send_ping(unsigned short port);
        void handle_ping(unsigned short port, void *packet, unsigned short size);
        void handle_pong(unsigned short port, void *packet, unsigned short size);
        void check_neighbors();

        // DV协议相关方法
        void send_dv_update(bool triggered = false);
        void handle_dv_packet(unsigned short port, void *packet, unsigned short size);
        bool update_dv_entry(unsigned short dest, unsigned short next_hop, 
                            unsigned short port, unsigned short cost);
        void check_dv();
        void forward_dv_data_packet(unsigned short port, void *packet, unsigned short size);
        void print_dv_table(); 

        // LS协议相关方法
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

