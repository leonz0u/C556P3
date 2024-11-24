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

/**
 * @brief 初始化路由协议实现
 * 
 * 初始化路由器的基本参数，设置定时器，启动邻居发现机制，
 * 并根据协议类型(DV/LS)初始化相应的路由表和更新机制。
 * 
 * @param num_ports 路由器端口数量
 * @param router_id 本路由器的ID
 * @param protocol_type 使用的路由协议类型(P_DV或P_LS)
 */
void RoutingProtocolImpl::init(unsigned short num_ports, unsigned short router_id, 
                              eProtocolType protocol_type) {
    // 保留原有初始化代码
    this->num_ports = num_ports;
    this->router_id = router_id;
    this->protocol_type = protocol_type;
    ports.resize(num_ports);
    
    // 设置周期性检查定时器(每秒检查一次)
    AlarmType *alarm_data = new AlarmType(ALARM_PERIODIC_CHECK);
    sys->set_alarm(this, 1000, (void *)alarm_data);
    
    // 初始化DV表中到自身的路由
    DVEntry self_entry;
    self_entry.cost = 0;
    self_entry.next_hop = router_id;
    self_entry.port = INFINITY_COST; // 表示本地
    self_entry.last_updated = sys->time();
    dv_table[router_id] = self_entry;

    // 立即发送第一轮PING
    for (unsigned short port = 0; port < num_ports; port++) {
        send_ping(port);
        // 设置后续的PING定时器
        AlarmType *ping_alarm = new AlarmType(ALARM_PING);
        unsigned short *port_data = new unsigned short(port);
        // 组合端口号和定时器类型
        void *data = new char[sizeof(AlarmType) + sizeof(unsigned short)];
        memcpy(data, ping_alarm, sizeof(AlarmType));
        memcpy((char*)data + sizeof(AlarmType), port_data, sizeof(unsigned short));
        sys->set_alarm(this, 10000, data);
        delete ping_alarm;
        delete port_data;
    }

    // 立即发送第一次DV更新
    if (protocol_type == P_DV) {
        send_dv_update(false);
        // 设置后续的DV更新定时器
        alarm_data = new AlarmType(ALARM_DV_UPDATE);
        sys->set_alarm(this, 30000, (void *)alarm_data);
    }  else if (protocol_type == P_LS) {
        // 初始化LS协议：发送第一轮链路状态更新
        send_ls_update(false);

        // 设置周期性LS更新定时器 (30秒)
        AlarmType *ls_update_alarm = new AlarmType(ALARM_LS_UPDATE);
        sys->set_alarm(this, 30000, (void *)ls_update_alarm);
    }
}


/**
 * @brief 向指定端口发送PING消息
 * 
 * 创建并发送PING数据包，用于发现邻居路由器和测量链路RTT。
 * 数据包包含发送时间戳，用于后续计算RTT。
 * 
 * @param port 目标端口号
 */
void RoutingProtocolImpl::send_ping(unsigned short port) {
    DEBUG_PRINT("Router %d sending PING on port %d\n", router_id, port);
    // 创建PING包
    // Packet Format: | type(1) | reserved(1) | size(2) | src_id(2) | dst_id(2) | payload(4) |
    unsigned short packet_size = 12; // 总大小12字节
    char *packet = new char[packet_size];
    
    // 设置包类型为PING
    packet[0] = PING;
    packet[1] = 0; // reserved
    
    // 设置包大小 (network byte order)
    unsigned short size_n = htons(packet_size);
    memcpy(packet + 2, &size_n, 2);
    
    // 设置源地址为本路由器ID
    unsigned short src_n = htons(router_id);
    memcpy(packet + 4, &src_n, 2);
    
    // 设置目标地址为0表示广播
    unsigned short dst_n = 0;
    memcpy(packet + 6, &dst_n, 2);
    
    // 设置时间戳作为payload
    unsigned int current_time = sys->time();
    memcpy(packet + 8, &current_time, 4);
    
    // 记录发送时间
    ports[port].last_ping_time = current_time;
    
    // 发送数据包
    sys->send(port, packet, packet_size);
    DEBUG_PRINT("Router %d PING sent with timestamp %u\n", router_id, current_time);
}

void RoutingProtocolImpl::handle_ping(unsigned short port, void *packet, unsigned short size) {
    // 将PING包转换为PONG包并返回
    char *pkt = (char *)packet;
    
    // 修改包类型为PONG
    pkt[0] = PONG;
    
    // 交换源地址和目标地址
    unsigned short src_id, dst_id;
    memcpy(&src_id, pkt + 4, 2);
    memcpy(&dst_id, pkt + 6, 2);
    
    src_id = ntohs(src_id);
    dst_id = ntohs(dst_id);

    DEBUG_PRINT("Router %d received PING from Router %d on port %d\n", 
                router_id, src_id, port);
    
    // 记录邻居ID
    if (ports[port].neighbor_id == INFINITY_COST) {
        ports[port].neighbor_id = src_id;
    }
    
    // 设置新的源和目标
    unsigned short new_src = htons(router_id);
    unsigned short new_dst = htons(src_id);
    memcpy(pkt + 4, &new_src, 2);
    memcpy(pkt + 6, &new_dst, 2);
    
    // 发送PONG响应
    sys->send(port, pkt, size);
}

/**
 * @brief 处理接收到的PONG响应
 * 
 * 处理PING的响应包，计算RTT，更新端口状态和邻居信息。
 * 如果发现拓扑变化或链路成本变化，会触发路由更新。
 * 
 * @param port 接收PONG的端口号
 * @param packet PONG数据包指针
 * @param size 数据包大小
 */
void RoutingProtocolImpl::handle_pong(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    unsigned int current_time = sys->time();
    
    // 获取发送PING的时间
    unsigned int ping_time;
    memcpy(&ping_time, pkt + 8, 4);
    
    // 计算RTT
    unsigned short rtt = current_time - ping_time;
    
    // 获取并记录邻居ID
    unsigned short src_id;
    memcpy(&src_id, pkt + 4, 2);
    src_id = ntohs(src_id);
    
    // 检查是否是新邻居或成本变化
    bool topology_changed = false;
    bool cost_changed = false;
    
    if (!ports[port].is_alive || ports[port].neighbor_id != src_id) {
        topology_changed = true;
    }
    if (ports[port].cost != rtt) {
        cost_changed = true;
    }
    
    // 更新端口状态
    ports[port].last_pong_time = current_time;
    ports[port].is_alive = true;
    ports[port].neighbor_id = src_id;
    ports[port].cost = rtt;
    
    DEBUG_PRINT("Router %d received PONG from Router %d on port %d, RTT = %u ms\n", 
                router_id, src_id, port, rtt);
    
    // 如果使用DV协议，更新到邻居的路由信息
    if (protocol_type == P_DV) {
        // 更新到邻居的路由信息update_dv_entry
        // path cost not rtt because rtt is recorded in port.cost
        bool route_updated = update_dv_entry(src_id, src_id, port, 0);
        
        // 如果是新邻居或路由有更新，触发DV更新
        if (topology_changed || route_updated) {
            send_dv_update(true);
            DEBUG_PRINT("Router %d: Triggered DV update due to neighbor change/update %d\n", 
                       router_id, src_id);
        }
    }

    if (protocol_type == P_LS && (topology_changed || cost_changed)) {
        // 更新链路状态数据库
        auto link_key = std::make_pair(std::min(router_id, src_id), std::max(router_id, src_id));
        LSEntry &lsa = ls_database[link_key];

        // 如果是新的 LSA 或者链路状态发生了变化，递增序列号
        if (lsa.seq_num == 0 || lsa.cost != rtt) {
            lsa.seq_num++;
        }

        lsa.src = router_id;
        lsa.dst = src_id;
        lsa.cost = rtt;
        lsa.last_updated = sys->time();

        DEBUG_PRINT("Router %d: Updated LS database for link (%d -> %d) with cost %u and seq_num %u\n",
                    router_id, router_id, src_id, rtt, lsa.seq_num);

        // 触发链路状态更新广播
        send_ls_update(true);
        DEBUG_PRINT("Router %d: Triggered LS update due to change in link (%d -> %d)\n",
                    router_id, router_id, src_id);
    }

    delete[] pkt;
}

/**
 * @brief 检查邻居状态
 * 
 * 周期性检查所有端口的邻居状态。
 * 如果15秒未收到PONG响应，则认为邻居断开连接。
 * 发现链路状态变化时会触发路由更新。
 */
void RoutingProtocolImpl::check_neighbors() {
    unsigned int current_time = sys->time();
    bool topology_changed = false;
    
    for (unsigned short port = 0; port < num_ports; port++) {
        if (ports[port].last_pong_time > 0) {  // 如果端口曾经收到过PONG
            unsigned int time_since_last_pong = current_time - ports[port].last_pong_time;
            
            // 15秒没有收到PONG，认为链路断开
            if (time_since_last_pong >= 15000) {
                if (ports[port].is_alive) {
                    ports[port].is_alive = false;
                    topology_changed = true;
                    
                    // 记录失效的邻居ID
                    unsigned short failed_neighbor = ports[port].neighbor_id;
                    
 
                    // 更新DV表
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

                    // 如果是 LS 协议，直接删除链路状态条目
                    if (protocol_type == P_LS) {
                        auto link_key = std::make_pair(router_id, failed_neighbor);
                        if (ls_database.find(link_key) != ls_database.end()) {
                            ls_database.erase(link_key);  // 删除失效链路
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
    
    // 如果拓扑发生变化且使用DV协议，触发更新
    if (topology_changed && protocol_type == P_DV) {
        send_dv_update(true);
    }
    else if (protocol_type == P_LS) {
            // LS 协议触发链路状态广播
            send_ls_update(true);
    }
}

/**
 * @brief 处理定时器事件
 * 
 * 处理各种类型的定时器事件，包括：
 * - PING检测(10秒)
 * - DV更新(30秒)
 * - 周期性状态检查(1秒)
 * 
 * @param data 定时器数据，包含定时器类型和相关参数
 */
void RoutingProtocolImpl::handle_alarm(void *data) {
    // 解析alarm类型
    AlarmType alarm_type;
    memcpy(&alarm_type, data, sizeof(AlarmType));
    
    switch (alarm_type) {
        case ALARM_PING: {
            // 获取端口号
            unsigned short port;
            memcpy(&port, (char*)data + sizeof(AlarmType), sizeof(unsigned short));
            
            // 发送PING并设置下一次alarm
            send_ping(port);
            
            // 设置下一个PING定时器
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
                
                // 确保设置下一次30秒的更新
                AlarmType *new_alarm = new AlarmType(ALARM_DV_UPDATE);
                sys->set_alarm(this, 30000, (void *)new_alarm);
            }
            break;
        }

        case ALARM_LS_UPDATE:
            if (protocol_type == P_LS) {
                DEBUG_PRINT("Router %d: Sending periodic LS update at time %u\n", 
                           router_id, sys->time());
                send_ls_update(false);
                AlarmType *new_alarm = new AlarmType(ALARM_LS_UPDATE);
                sys->set_alarm(this, 30000, (void *)new_alarm);
            }
            break;
        
        case ALARM_PERIODIC_CHECK: {
            // 执行所有周期性检查
            check_neighbors();  // 检查邻居状态
            
            if (protocol_type == P_DV) {
                check_dv();  // 检查路由超时
            } else if(protocol_type == P_LS){
                // check_ls_timeouts();
                check_ls();
            }
            
            // 设置下一次周期性检查
            AlarmType *new_alarm = new AlarmType(ALARM_PERIODIC_CHECK);
            sys->set_alarm(this, 1000, (void *)new_alarm);
            break;
        }
    }
    
    // 清理alarm数据
    delete[] (char*)data;
}

/**
 * @brief 处理接收到的数据包
 * 
 * 根据数据包类型进行不同的处理：
 * - PING: 处理邻居发现请求
 * - PONG: 处理邻居发现响应
 * - DV: 处理距离向量路由更新
 * - DATA: 处理数据包转发
 * 
 * 特殊情况：
 * - 当port为SPECIAL_PORT时，表示数据包是本地生成的
 * - 当收到目的地为本机的数据包时，直接处理而不转发
 * 
 * @param port 接收数据包的端口号，SPECIAL_PORT表示本地生成的包
 * @param packet 数据包指针器
 * @param size 数据包大小(字节)
 */
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
                // 本地产生的数据包
                if (protocol_type == P_DV) {
                    forward_dv_data_packet(port, packet, size);
                } else if(protocol_type == P_LS) {
                    forward_ls_data_packet(port, packet, size);
                }
            } else {
                // 收到需要转发的数据包
                unsigned short dst_id;
                memcpy(&dst_id, pkt + 6, 2);
                dst_id = ntohs(dst_id);
                
                if (dst_id == router_id) {
                    // 到达目的地，释放包
                    delete[] pkt;
                } else {
                    // 需要转发
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

/**
 * @brief 发送距离向量更新
 * 
 * 向所有活跃端口的邻居发送当前的路由信息。
 * 实现水平分裂和毒性逆转机制以防止路由环路。
 * 
 * @param triggered 是否为触发更新，true表示立即更新，false表示周期性更新, 调试输出使用
 */
void RoutingProtocolImpl::send_dv_update(bool triggered) {
    DEBUG_PRINT("Router %d: Preparing DV update at time %u (triggered=%d)\n", 
                router_id, sys->time(), triggered);
    
    // 基本DV包头: type(1) + reserved(1) + size(2) + src_id(2) + dst_id(2)
    unsigned short base_size = 8;
    // 每个DV表项: dest_id(2) + cost(2)
    unsigned short entry_size = 4;
    
    // 遍历所有端口
    for (unsigned short port = 0; port < num_ports; port++) {
        // 检查端口是否已经收到过PONG（表示有邻居）
        if (ports[port].neighbor_id == INFINITY_COST) {
            continue;  // 跳过没有邻居的端口
        }
        
        // 收集要发送的路由表项
        std::vector<std::pair<unsigned short, unsigned short>> entries;
        
        // 添加所有路由条目，包括到自己的路由
        for (const auto &entry : dv_table) {
            unsigned short cost;
            if (entry.second.port == port) {
                // 毒性逆转：如果路由通过这个端口，通告无穷大
                cost = INFINITY_COST;
            } else {
                cost = entry.second.cost;
            }
            
            entries.push_back({entry.first, cost});
        }
        
        // 即使没有表项也发送更新（至少包含到自己的路由）
        unsigned short packet_size = base_size + entry_size * entries.size();
        char *packet = new char[packet_size];
        memset(packet, 0, packet_size);  // 清零整个包
        
        // 设置包头
        packet[0] = DV;
        packet[1] = 0; // reserved
        unsigned short size_n = htons(packet_size);
        memcpy(packet + 2, &size_n, 2);
        unsigned short src_n = htons(router_id);
        memcpy(packet + 4, &src_n, 2);
        unsigned short dst_n = htons(ports[port].neighbor_id);
        memcpy(packet + 6, &dst_n, 2);
        
        // 添加DV表项
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
        // 注意：不要在这里删除packet，因为send()函数会接管内存
    }
}

/**
 * @brief 发送链路状态更新
 * 
 * 向所有活跃端口的邻居发送当前的链路状态信息。
 * 实现洪泛机制，包含所有链路状态条目。
 * 
 * @param triggered 是否为触发更新，true表示立即更新，false表示周期性更新, 调试输出使用
 */
void RoutingProtocolImpl::send_ls_update(bool triggered) {
    DEBUG_PRINT("Router %d: Preparing LS update at time %u (triggered=%d)\n",
                router_id, sys->time(), triggered);

    // 基本LS包头: type(1) + reserved(1) + size(2) + src_id(2)
    unsigned short base_size = 8;
    // 每个LSA: src_id(2) + dst_id(2) + cost(2) + seq_num(4)
    unsigned short entry_size = 10;

    // 构建要发送的 LSA 列表，只包括成本不为 INFINITY_COST 的 LSAs
    std::vector<LSEntry> lsas;

    for (const auto &entry_pair : ls_database) {
        const LSEntry &lsa = entry_pair.second;
        if (lsa.cost != INFINITY_COST) {
            lsas.push_back(lsa);
        }
    }

    // 如果没有 LSA，需要退出以避免发送空包
    if (lsas.empty()) {
        DEBUG_PRINT("Router %d: No LSAs to send, aborting LS update.\n", router_id);
        return;
    }

    // 构建并发送 LS 更新包
    unsigned short packet_size = base_size + entry_size * lsas.size();
    char *packet = new char[packet_size];
    memset(packet, 0, packet_size);  // 清零整个包

    // 设置包头
    packet[0] = LS;  // 包类型
    packet[1] = 0;   // 保留字段
    unsigned short size_n = htons(packet_size);
    memcpy(packet + 2, &size_n, 2);
    unsigned short src_n = htons(router_id);
    memcpy(packet + 4, &src_n, 2);

    // 添加 LSA
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

    // 发送给所有邻居
    for (unsigned short port = 0; port < num_ports; port++) {
        if (!ports[port].is_alive) {
            continue;  // 跳过非活跃端口
        }

        // 创建包的副本发送
        char *packet_copy = new char[packet_size];
        memcpy(packet_copy, packet, packet_size);
        sys->send(port, packet_copy, packet_size);

        DEBUG_PRINT("Router %d: Sent LS update to neighbor %d on port %d\n",
                    router_id, ports[port].neighbor_id, port);
    }

    delete[] packet;  // 释放原始包
}



/**
 * @brief 处理接收到的DV更新包
 * 
 * 处理邻居发送的距离向量更新信息，更新本地路由表。
 * 如果路由信息发生变化，可能触发新的DV更新。
 * 
 * @param port 接收更新的端口号
 * @param packet DV更新包指针
 * @param size 数据包大小
 */
void RoutingProtocolImpl::handle_dv_packet(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    unsigned short src_id;
    memcpy(&src_id, pkt + 4, 2);
    src_id = ntohs(src_id);
    
    DEBUG_PRINT("Router %d: Received DV update from Router %d on port %d\n", 
                router_id, src_id, port);
    
    // 确保这是从正确的邻居收到的更新
    if (ports[port].neighbor_id != src_id) {
        DEBUG_PRINT("Router %d: Ignoring DV update from unexpected neighbor %d on port %d\n",
                   router_id, src_id, port);
        delete[] pkt;
        return;
    }
    
    bool updated = false;
    // unsigned int current_time = sys->time();
    
    // 从包中提取DV表项
    int offset = 8;
    while (offset < size) {
        unsigned short dest, cost;
        memcpy(&dest, pkt + offset, 2);
        memcpy(&cost, pkt + offset + 2, 2);
        dest = ntohs(dest);
        cost = ntohs(cost);
        
        // 不处理到自己的路由更新
        if (dest == router_id) {
            offset += 4;
            continue;
        }
        
        // 更新DV表
        updated |= update_dv_entry(dest, src_id, port, cost);
        
        offset += 4;
    }
    
    // 只有在实际发生更新时才触发更新发送
    if (updated) {
        DEBUG_PRINT("Router %d: Triggering DV update due to route changes\n", router_id);
        send_dv_update(true);
    }
    
    delete[] pkt;
}

/**
 * @brief 处理接收到的LS更新包
 * 
 * 处理邻居发送的链路状态更新信息，更新本地链路状态数据库。
 * 如果链路状态发生变化，可能触发新的最短路径计算。
 * 
 * @param port 接收更新的端口号
 * @param packet LS更新包指针
 * @param size 数据包大小
 */
void RoutingProtocolImpl::handle_ls_packet(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    unsigned short src_id;
    memcpy(&src_id, pkt + 4, 2);
    src_id = ntohs(src_id);

    DEBUG_PRINT("Router %d: Received LS update from Router %d on port %d\n",
                router_id, src_id, port);

    // Ensure this is from the expected neighbor
    if (ports[port].neighbor_id != src_id) {
        DEBUG_PRINT("Router %d: Ignoring LS update from unexpected neighbor %d on port %d\n",
                    router_id, src_id, port);
        delete[] pkt;
        return;
    }

    bool updated = false;        // 标记是否需要重新计算最短路径
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

        // 跳过成本为 INFINITY_COST 的 LSA
        if (lsa_cost == INFINITY_COST) {
            offset += 10;
            continue;
        }

        // Determine if we need to update the LS database
        auto link_key = std::make_pair(std::min(lsa_src, lsa_dst), std::max(lsa_src, lsa_dst));
        bool need_update = false;
        bool cost_changed = false;  // 新增变量，标记成本是否发生变化

        auto it = ls_database.find(link_key);
        if (it == ls_database.end()) {
            // LSDB 中没有该 LSA，需要添加并可能重新计算最短路径
            need_update = true;
            cost_changed = true;  // 新的 LSA，成本视为已变化
        } else {
            LSEntry &existing_lsa = it->second;

            if (seq_num > existing_lsa.seq_num) {
                // 收到更新的 LSA，比较成本
                need_update = true;
                if (lsa_cost != existing_lsa.cost) {
                    cost_changed = true;  // 成本发生变化
                } else {
                    cost_changed = false; // 成本未变化
                }
            } else if (seq_num == existing_lsa.seq_num && lsa_cost != existing_lsa.cost) {
                // 序列号相同但成本不同，可能存在问题，但仍更新
                need_update = true;
                cost_changed = true;
            } else {
                // 序列号不更高，或者成本未变化，不需要更新
                need_update = false;
            }
        }

        if (need_update) {
            // Update the link state entry in LSDB
            LSEntry& ls_entry = ls_database[link_key];
            ls_entry.src = lsa_src;
            ls_entry.dst = lsa_dst;
            ls_entry.cost = lsa_cost;
            ls_entry.seq_num = seq_num;
            ls_entry.last_updated = current_time;

            if (cost_changed) {
                updated = true;  // 成本发生变化，需要重新计算最短路径
                DEBUG_PRINT("Router %d: Updated link state (%d -> %d) with cost %d and seq_num %u\n",
                            router_id, lsa_src, lsa_dst, lsa_cost, seq_num);
            } else {
                // 成本未变化，不需要重新计算最短路径
                DEBUG_PRINT("Router %d: Received newer LSA (%d -> %d) with same cost %d and higher seq_num %u\n",
                            router_id, lsa_src, lsa_dst, lsa_cost, seq_num);
            }
        }

        offset += 10; // Each LSA is 10 bytes
    }

    // If an update occurred that changed costs, recompute shortest paths
    if (updated) {
        DEBUG_PRINT("Router %d: Triggering shortest path computation due to LS update\n", router_id);
        compute_shortest_paths();
    }

    // Implement flooding mechanism
    for (unsigned short p = 0; p < num_ports; p++) {
        if (p == port) {
            continue;  // Do not flood back to sender
        }
        if (!ports[p].is_alive) {
            continue;  // Skip inactive ports
        }

        // Send a copy of the packet
        char *pkt_copy = new char[size];
        memcpy(pkt_copy, pkt, size);
        sys->send(p, pkt_copy, size);

        DEBUG_PRINT("Router %d: Flooded LS update to neighbor %d on port %d\n",
                    router_id, ports[p].neighbor_id, p);
    }

    delete[] pkt;
}




void RoutingProtocolImpl::compute_shortest_paths() {
    // 构建节点集合
    std::set<unsigned short> nodes;
    for (const auto& entry : ls_database) {
        nodes.insert(entry.second.src);
        nodes.insert(entry.second.dst);
    }

    // 初始化距离和前驱节点
    std::map<unsigned short, unsigned int> dist;
    std::map<unsigned short, unsigned short> prev;
    for (auto node : nodes) {
        dist[node] = UINT_MAX; // 使用无穷大表示不可达
        prev[node] = INFINITY_COST;
    }
    dist[router_id] = 0;

    // Dijkstra 算法
    std::set<unsigned short> unvisited = nodes;

    while (!unvisited.empty()) {
        // 找到未访问节点中距离最小的节点
        unsigned short min_node = INFINITY_COST;
        unsigned int min_dist = UINT_MAX;

        for (auto node : unvisited) {
            if (dist[node] < min_dist) {
                min_dist = dist[node];
                min_node = node;
            }
        }

        if (min_node == INFINITY_COST) {
            // 剩余的节点不可达，退出循环
            break;
        }

        unvisited.erase(min_node);

        // 更新邻居节点的距离
        for (const auto& entry_pair : ls_database) {
            const LSEntry& ls_entry = entry_pair.second;
            unsigned short neighbor = INFINITY_COST;
            unsigned int cost = ls_entry.cost;

            // 跳过成本为 INFINITY_COST 的链路
            if (cost == INFINITY_COST) {
                continue;
            }

            if (ls_entry.src == min_node) {
                neighbor = ls_entry.dst;
            } else if (ls_entry.dst == min_node) {
                neighbor = ls_entry.src;
            } else {
                continue;
            }

            if (unvisited.find(neighbor) != unvisited.end()) {
                unsigned int alt = dist[min_node] + cost;
                if (alt < dist[neighbor]) {
                    dist[neighbor] = alt;
                    prev[neighbor] = min_node;
                }
            }
        }
    }

    // 更新 LS 路由表
    lsRoutingTable.clear(); // 清除旧的路由信息

    for (auto node : nodes) {
        if (node == router_id || dist[node] == UINT_MAX) {
            continue; // 跳过自己和不可达的节点
        }

        // 通过前驱节点链找到下一跳
        unsigned short next_hop = node;
        while (prev[next_hop] != router_id && prev[next_hop] != INFINITY_COST) {
            next_hop = prev[next_hop];
        }

        if (prev[next_hop] == INFINITY_COST) {
            // 无法找到路径，跳过
            continue;
        }

        // 找到与下一跳相连的端口
        unsigned short port = INFINITY_COST;
        for (unsigned short p = 0; p < num_ports; p++) {
            if (ports[p].neighbor_id == next_hop && ports[p].is_alive) {
                port = p;
                break;
            }
        }

        if (port == INFINITY_COST) {
            // 未找到有效的端口，跳过
            continue;
        }

        // 更新 LS 路由表
        LSRouteEntry route_entry;
        route_entry.next_hop = next_hop;
        route_entry.port = port;
        route_entry.cost = dist[node];
        lsRoutingTable[node] = route_entry;
    }

    // 可选：打印更新后的 LS 路由表
    print_ls_routing_table();
}




void RoutingProtocolImpl::check_dv() {
    unsigned int current_time = sys->time();
    // bool updated = false;
    std::vector<unsigned short> to_delete;
    
    // 首先标记所有超时的路由为INFINITY_COST
    for (auto &entry : dv_table) {
        if (entry.first != router_id) {  // 不处理到自身的路由
            if (current_time - entry.second.last_updated >= 45000 || // 45秒超时
                entry.second.cost == INFINITY_COST) {               // 已标记为无效
                to_delete.push_back(entry.first);
                // updated = true;
            }
        }
    }
    
    // 统一删除所有无效路由
    for (const auto &dest : to_delete) {
        dv_table.erase(dest);
        DEBUG_PRINT("Router %d: Removed invalid route to %d\n", router_id, dest);
    }
    
    // if (updated) {
    //     send_dv_update(true);

// }
}

void RoutingProtocolImpl::check_ls() {
    unsigned int current_time = sys->time();
    bool updated = false;

    for (auto it = ls_database.begin(); it != ls_database.end();) {
        if (current_time - it->second.last_updated > 45000) {
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
        send_ls_update(true);
    }
}


/**
 * @brief 转发数据包
 * 
 * 根据路由表转发数据包到适当的下一跳。
 * 如果没有到目的地的路由，则丢弃数据包。
 * 
 * @param port 接收数据包的端口号
 * @param packet 数据包指针
 * @param size 数据包大小
 */
void RoutingProtocolImpl::forward_dv_data_packet(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    unsigned short dst_id;
    memcpy(&dst_id, pkt + 6, 2);
    dst_id = ntohs(dst_id);
    
    // 查找路由表
    auto it = dv_table.find(dst_id);
    if (it != dv_table.end() && it->second.port != INFINITY_COST) {
        // 创建新的包副本
        char *pkt_copy = new char[size];
        memcpy(pkt_copy, pkt, size);
        sys->send(it->second.port, pkt_copy, size);
    } else {
        // 没有路由，丢弃包
        delete[] pkt;
    }
}

void RoutingProtocolImpl::forward_ls_data_packet(unsigned short port, void *packet, unsigned short size) {
    char *pkt = (char *)packet;
    unsigned short dst_id;
    memcpy(&dst_id, pkt + 6, 2);
    dst_id = ntohs(dst_id);

    // 查找路由信息
    auto it = lsRoutingTable.find(dst_id);
    if (it != lsRoutingTable.end() && it->second.port != INFINITY_COST) {
        // 创建新的包副本
        char *pkt_copy = new char[size];
        memcpy(pkt_copy, pkt, size);
        sys->send(it->second.port, pkt_copy, size);
        delete[] pkt; // 释放原始数据包
    } else {
        // 没有路由，丢弃包
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


/**
 * @brief 更新距离向量路由表项
 * 
 * 根据新的路由信息更新DV路由表。处理新路由添加、路由更新和路由失效。
 * 只有当新路由更优或当前路由状态变化时才进行更新。
 * 
 * @param dest 目的地路由器ID
 * @param next_hop 下一跳路由器ID
 * @param port 出口端口号
 * @param path_cost 到目的地的成本
 * @return bool 如果路由表发生更新返回true，否则返回false
 */
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
    
    // if (updated) {
    //     print_dv_table();
    // }
    
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