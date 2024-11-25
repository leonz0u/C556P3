# COMP 556 - Introduction to Computer Networks - Project 3 - Intra-Domain Routing Protocols for Bisco GSR9999


This project is a group work of four MCS students, including Haoran Zhang(hz115), Chengxuan Zou(cz76), Zilong Xue(zx55) and Zhenhua Zhang(zz123).


---

To test the intradomain routing policy of our implementation, you can run the following commands in the terminal:


## Building and Runing

1. route to the current working directory: `cd /path/to/project3`

2. build the project with our Makefile:`make`

3. run the Simulator with specifed policy: `./Simulator <Configuration File> <specified policy>`

For instance, if you want to run the Simulator with Link State Policy with simpletest1, you should type:`./Simulator simpletest1 LS`; 

If you want to run the Simulator with Distance Vector Policy with simpletest2, you should type:`./Simulator simpletest2 DV`.

## Things grader should know

First, you should review README.md and follow the instructions to check the operation of our implementation.

Then, If you want to know about how we implement the routing policy through our code, you can check the following: 

# In the RoutingProtocollmpl.h
## Enum `AlarmType`
Defines various timer types used for scheduling different tasks:
- **`ALARM_PING`**: Triggers a PING every 10 seconds.
- **`ALARM_DV_UPDATE`**: Triggers a Distance Vector (DV) update every 30 seconds.
- **`ALARM_LS_UPDATE`**: Triggers a Link State (LS) update every 30 seconds.
- **`ALARM_PERIODIC_CHECK`**: Triggers a periodic status check every 1 second, including neighbor and route checks.

---

## Structure `PortStatus`
Holds the status information for each router port:
- **`neighbor_id`**: The ID of the neighbor router, initially set to `INFINITY_COST` (indicating no neighbor).
- **`last_ping_time`**: The last time a PING was sent on this port.
- **`last_pong_time`**: The last time a PONG was received on this port.
- **`is_alive`**: A flag indicating whether the port is active.
- **`cost`**: The link cost, represented as RTT (Round-Trip Time).

---

## Structure `DVEntry`
Represents an entry in the Distance Vector routing table:
- **`next_hop`**: The next-hop router ID to reach the destination.
- **`port`**: The outgoing port used to forward packets.
- **`cost`**: The total cost to reach the destination.
- **`last_updated`**: The timestamp of the last update for this entry.

---

## Structure `LSEntry`
Stores information about a link in the Link State database:
- **`src`**: The source router ID of the link.
- **`dst`**: The destination router ID of the link.
- **`cost`**: The cost of the link.
- **`seq_num`**: The sequence number of the link-state advertisement, used to prevent duplicate processing.
- **`last_updated`**: The timestamp of the last update for this link entry.

---

## Structure `LSRouteEntry`
Represents an entry in the Link State routing table:
- **`next_hop`**: The next-hop router ID to reach the destination.
- **`port`**: The outgoing port used to forward packets.
- **`cost`**: The total cost to reach the destination.

---

## Member Variables
- **`Node *sys`**: The system interface for interacting with the simulator.
- **`unsigned short router_id`**: The ID of the current router.
- **`unsigned short num_ports`**: The number of ports on the router.
- **`eProtocolType protocol_type`**: The protocol type in use (`P_DV` for Distance Vector or `P_LS` for Link State).
- **`std::vector<PortStatus> ports`**: A table tracking the status of each port.
- **`std::map<unsigned short, DVEntry> dv_table`**: The Distance Vector routing table mapping destinations to next hops.
- **`LSDatabase ls_database`**: The Link State database storing information about all known links.
- **`ls_routing_table lsRoutingTable`**: The Link State routing table used to calculate the next hop and cost to each destination.

# In RoutingProtocollmph.cc
# Function Overview and Explanation

## Constructor and Destructor
1. **`RoutingProtocolImpl(Node *n)`**
   - Initializes the `RoutingProtocolImpl` object with a pointer to the system interface (`Node *n`).
   - Sets up the internal `sys` pointer for later interactions with the simulator.

2. **`~RoutingProtocolImpl()`**
   - Destructor. Currently does not perform any specific cleanup as memory management is done elsewhere.

---

## Initialization
3. **`void init(unsigned short num_ports, unsigned short router_id, eProtocolType protocol_type)`**
   - Initializes the routing protocol instance with the number of ports, router ID, and protocol type (`P_DV` or `P_LS`).
   - Schedules periodic checks and initializes necessary timers.
   - For DV, sets up the routing entry to itself and sends the first DV update.
   - For LS, prepares and sends the initial LS update.

---

## Packet Sending
4. **`void send_ping(unsigned short port)`**
   - Sends a PING packet on the specified port.
   - Updates the `last_ping_time` for the port to track when the PING was sent.

5. **`void send_dv_update(bool triggered)`**
   - Sends a Distance Vector (DV) update to all neighbors.
   - Handles poisoned reverse by advertising infinite cost for routes learned through the same port.

6. **`void send_ls_update(bool triggered)`**
   - Sends a Link State (LS) update containing all relevant Link State Advertisements (LSAs) to all neighbors.

---

## Packet Handling
7. **`void handle_ping(unsigned short port, void *packet, unsigned short size)`**
   - Responds to a received PING packet by converting it to a PONG and sending it back to the sender.
   - Updates the neighbor information if it's a new connection.

8. **`void handle_pong(unsigned short port, void *packet, unsigned short size)`**
   - Processes a PONG packet and updates the port status, including the neighbor's ID and the cost (RTT).
   - Triggers DV or LS updates if there are changes in topology or link costs.

9. **`void handle_dv_packet(unsigned short port, void *packet, unsigned short size)`**
   - Processes a received DV update packet.
   - Updates the DV table with new route information and triggers a DV update if necessary.

10. **`void handle_ls_packet(unsigned short port, void *packet, unsigned short size)`**
    - Processes a received LS update packet.
    - Updates the LS database and triggers LS flooding and shortest path computation if there are changes.

---

## Periodic and Alarm Handling
11. **`void handle_alarm(void *data)`**
    - Handles periodic alarms for various tasks like PING, DV updates, LS updates, and periodic checks.
    - Reschedules alarms as needed.

12. **`void check_neighbors()`**
    - Checks the status of all neighbors.
    - Marks neighbors as inactive if no PONG is received within 15 seconds and updates the topology.

13. **`void check_dv()`**
    - Periodically checks the DV table for expired routes (no updates in 45 seconds) and removes them.

14. **`void check_ls()`**
    - Periodically checks the LS database for expired LSAs (no updates in 45 seconds) and removes them.

---

## Routing and Forwarding
15. **`void forward_dv_data_packet(unsigned short port, void *packet, unsigned short size)`**
    - Forwards a received data packet using the DV routing table.
    - Drops the packet if no route is found.

16. **`void forward_ls_data_packet(unsigned short port, void *packet, unsigned short size)`**
    - Forwards a received data packet using the LS routing table.
    - Drops the packet if no route is found.

---

## LS and DV Management
17. **`void add_neighbor_SeqNum()`**
    - Increments the sequence numbers of LSAs related to the router and ensures outdated entries are removed.

18. **`void compute_shortest_paths()`**
    - Runs Dijkstra's algorithm to compute the shortest paths and updates the LS routing table.

19. **`bool update_dv_entry(unsigned short dest, unsigned short next_hop, unsigned short port, unsigned short path_cost)`**
    - Updates or adds an entry in the DV table based on new route information.
    - Checks for better paths or outdated routes.

---

## Debug and Logging
20. **`void print_dv_table()`**
    - Prints the current DV table for debugging purposes.

21. **`void print_ls_routing_table()`**
    - Prints the current LS routing table for debugging purposes.

---