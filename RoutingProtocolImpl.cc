#include "RoutingProtocolImpl.h"
#include <stdio.h>
#include <string.h>
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
    unsigned int rtt = current_time - ping_time;
    
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
    
    // 如果使用DV协议且是新邻居，确保添加初始路由条目
    if (protocol_type == P_DV && topology_changed) {
        // 更新到邻居的路由信息
        update_dv_entry(src_id, src_id, port, rtt);
        
        // 立即发送一个DV更新，因为我们有了新邻居
        send_dv_update(true);
        DEBUG_PRINT("Router %d: Triggered DV update due to new neighbor %d\n", 
                   router_id, src_id);
    }
    // 如果只是成本变化，也需要更新
    else if (protocol_type == P_DV && cost_changed) {
        // 更新到邻居的路由信息
        update_dv_entry(src_id, src_id, port, rtt);
        
        // 检查是否需要更新通过这个邻居的其他路由
        bool routes_updated = false;
        for (auto &entry : dv_table) {
            if (entry.second.next_hop == src_id) {
                // 重新计算通过这个邻居的路由成本
                unsigned short new_cost = rtt + 
                    (entry.first == src_id ? 0 : entry.second.cost - ports[port].cost);
                if (new_cost != entry.second.cost) {
                    entry.second.cost = new_cost;
                    entry.second.last_updated = current_time;
                    routes_updated = true;
                }
            }
        }
        
        // 如果有路由更新，触发DV更新
        if (routes_updated) {
            send_dv_update(true);
            DEBUG_PRINT("Router %d: Triggered DV update due to cost change to neighbor %d\n", 
                       router_id, src_id);
        }
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
                        // 移除通过该邻居的路由
                        auto it = dv_table.begin();
                        while (it != dv_table.end()) {
                            if (it->second.next_hop == failed_neighbor) {
                                it = dv_table.erase(it);
                            } else {
                                ++it;
                            }
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
        
        case ALARM_PERIODIC_CHECK: {
            // 执行所有周期性检查
            check_neighbors();  // 检查邻居状态
            
            if (protocol_type == P_DV) {
                check_dv_timeouts();  // 检查路由超时
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
        case DATA:
            if (port == SPECIAL_PORT) {
                // 本地产生的数据包
                forward_data_packet(port, packet, size);
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
                    forward_data_packet(port, packet, size);
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
    unsigned int current_time = sys->time();
    
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
        
        // 合法性检查
        if (cost != INFINITY_COST) {
            unsigned short total_cost = cost + ports[port].cost;
            
            // 检查是否会溢出
            if (total_cost < cost || total_cost < ports[port].cost) {
                DEBUG_PRINT("Router %d: Cost overflow detected for dest %d\n", router_id, dest);
                offset += 4;
                continue;
            }
            
            if (total_cost > INFINITY_COST) {
                total_cost = INFINITY_COST;
            }
            
            // 更新路由表
            bool route_updated = update_dv_entry(dest, src_id, port, total_cost);
            if (route_updated) {
                updated = true;
                DEBUG_PRINT("Router %d: Updated route to %d via %d (port %d) with cost %d\n",
                           router_id, dest, src_id, port, total_cost);
            }
        } else {
            // 处理无穷大的情况
            auto it = dv_table.find(dest);
            if (it != dv_table.end() && it->second.next_hop == src_id) {
                // 如果当前路由通过这个邻居，需要移除
                dv_table.erase(it);
                updated = true;
                DEBUG_PRINT("Router %d: Removed route to %d due to infinity cost from %d\n",
                           router_id, dest, src_id);
            }
        }
        
        offset += 4;
    }
    
    // 只有在实际发生更新时才触发更新发送
    if (updated) {
        DEBUG_PRINT("Router %d: Triggering DV update due to route changes\n", router_id);
        send_dv_update(true);
    }
    
    delete[] pkt;
}


void RoutingProtocolImpl::check_dv_timeouts() {
    unsigned int current_time = sys->time();
    bool updated = false;
    
    // 检查所有路由项
    auto it = dv_table.begin();
    while (it != dv_table.end()) {
        if (it->first != router_id && // 不检查到自身的路由
            current_time - it->second.last_updated >= 45000) { // 45秒超时
            it = dv_table.erase(it);
            updated = true;
        } else {
            ++it;
        }
    }
    
    if (updated) {
        send_dv_update(true);
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
void RoutingProtocolImpl::forward_data_packet(unsigned short port, void *packet, unsigned short size) {
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

/**
 * @brief 更新距离向量路由表项
 * 
 * 根据新的路由信息更新DV路由表。处理新路由添加、路由更新和路由失效。
 * 只有当新路由更优或当前路由状态变化时才进行更新。
 * 
 * @param dest 目的地路由器ID
 * @param next_hop 下一跳路由器ID
 * @param port 出口端口号
 * @param cost 到目的地的成本
 * @return bool 如果路由表发生更新返回true，否则返回false
 */
bool RoutingProtocolImpl::update_dv_entry(unsigned short dest, unsigned short next_hop,
                                        unsigned short port, unsigned short cost) {
    bool updated = false;
    unsigned int current_time = sys->time();
    
    // 不要更新到自己的路由
    if (dest == router_id) {
        return false;
    }
    
    auto it = dv_table.find(dest);
    if (it == dv_table.end()) {
        // 新路由
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
        // 检查是否真的需要更新
        bool need_update = false;
        if (cost == INFINITY_COST && it->second.next_hop == next_hop) {
            // 当前路径变为无效
            need_update = true;
        } else if (cost != INFINITY_COST && 
                  (cost < it->second.cost ||                    // 更好的路径
                   (it->second.next_hop == next_hop &&         // 当前路径的更新
                    cost != it->second.cost))) {               // 且成本有变化
            need_update = true;
        }
        
        if (need_update) {
            it->second.next_hop = next_hop;
            it->second.port = port;
            it->second.cost = cost;
            it->second.last_updated = current_time;
            updated = true;
            DEBUG_PRINT("Router %d: Updating route to %d via %d with cost %d\n",
                       router_id, dest, next_hop, cost);
        }
    }
    
    if (updated) {
        print_dv_table();
    }
    
    return updated;
}

void RoutingProtocolImpl::print_dv_table() {
    DEBUG_PRINT("\nRouter %d DV Table:\n", router_id);
    DEBUG_PRINT("Dest\tNextHop\tPort\tCost\tLastUpdated\n");
    for (const auto &entry : dv_table) {
        DEBUG_PRINT("%d\t%d\t%d\t%d\t%u\n",
                   entry.first,
                   entry.second.next_hop,
                   entry.second.port,
                   entry.second.cost,
                   entry.second.last_updated);
    }
    DEBUG_PRINT("\n");
}