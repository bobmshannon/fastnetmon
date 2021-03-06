/* Author: pavel.odintsov@gmail.com */
/* License: GPLv2 */

#include <stdio.h> 
#include <stdlib.h> 
#include <errno.h> 
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <math.h> 

#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <netdb.h>

#include "libpatricia/patricia.h"
#include "fastnetmon_types.h"
#include "sflow_plugin/sflow_collector.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <fstream>

#include <vector>
#include <utility>
#include <sstream>

#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/regex.hpp>

// log4cpp logging facility
#include "log4cpp/Category.hh"
#include "log4cpp/Appender.hh"
#include "log4cpp/FileAppender.hh"
#include "log4cpp/OstreamAppender.hh"
#include "log4cpp/Layout.hh"
#include "log4cpp/BasicLayout.hh"
#include "log4cpp/PatternLayout.hh"
#include "log4cpp/Priority.hh"

// Boost libs
#include <boost/algorithm/string.hpp>

#ifdef GEOIP
#include "GeoIP.h"
#endif

#ifdef PCAP
#include <pcap.h>
#endif

#ifdef REDIS
#include <hiredis/hiredis.h>
#endif

#ifdef PF_RING
#include "pfring.h"
#include "pfring_zc.h"
#include <numa.h>
#endif

using namespace std;

/* 802.1Q VLAN tags are 4 bytes long. */
#define VLAN_HDRLEN 4

/* Complete list of ethertypes: http://en.wikipedia.org/wiki/EtherType */
/* This is the decimal equivalent of the VLAN tag's ether frame type */
#define VLAN_ETHERTYPE 0x8100
#define IP_ETHERTYPE 0x0800
#define IP6_ETHERTYPE 0x86dd
#define ARP_ETHERTYPE 0x0806

// Interface name or interface list (delimitered by comma)
string work_on_interfaces = "";

string global_config_path = "/etc/fastnetmon.conf";

boost::regex regular_expression_cidr_pattern("^\\d+\\.\\d+\\.\\d+\\.\\d+\\/\\d+$");

time_t last_call_of_traffic_recalculation;

// We can look inside L2TP packets with IP encapsulation
// And do it by default
bool do_unpack_l2tp_over_ip = true;

// Variable from PF_RING multi channel mode
int num_pfring_channels = 0;

// Variable with all data from main screen
string screen_data_stats = "";

// We can use software or hardware (in kernel module) packet parser
bool we_use_pf_ring_in_kernel_parser = true;

// By default we pool PF_RING on one thread
bool enable_pfring_multi_channel_mode = false;

// We can use ZC api
bool pf_ring_zc_api_mode = false;

u_int32_t zc_num_threads = 0;

/* Configuration block, we must move it to configuration file  */
#ifdef REDIS
unsigned int redis_port = 6379;
string redis_host = "127.0.0.1";
// because it's additional and very specific feature we should disable it by default
bool redis_enabled = false;
#endif

bool enable_ban_for_pps = false;
bool enable_ban_for_bandwidth = false;
bool enable_ban_for_flows_per_second = false;
 
bool enable_conection_tracking = true;

bool enable_data_collection_from_mirror = true;

bool enable_sflow_collection = false;

// Time consumed by reaclculation for all IPs
struct timeval speed_calculation_time;

// Time consumed by drawing stats for all IPs
struct timeval drawing_thread_execution_time;

// Total number of hosts in our networks
// We need this as global variable because it's very important value for configuring data structures
unsigned int total_number_of_hosts_in_our_networks = 0;

#ifdef GEOIP
GeoIP * geo_ip = NULL;
#endif

patricia_tree_t *lookup_tree, *whitelist_tree;

bool DEBUG = 0;

// flag about dumping all packets to log
bool DEBUG_DUMP_ALL_PACKETS = false;

// Period for update screen for console version of tool
unsigned int check_period = 3;

// Standard ban time in seconds for all attacks but you can tune this value
int standard_ban_time = 1800; 

// We calc average pps/bps for this time
double average_calculation_amount = 15;

// Enable or disable average traffic counter
bool print_average_traffic_counts = false;

#ifdef PCAP
// Enlarge receive buffer for PCAP for minimize packet drops
unsigned int pcap_buffer_size_mbytes = 10;
#endif

// Key used for sorting clients in output.  Allowed sort params: packets/bytes
string sort_parameter = "packets";

// Path to notify script 
string notify_script_path = "/usr/local/bin/notify_about_attack.sh";

// Number of lines in programm output
unsigned int max_ips_in_list = 7;

// We must ban IP if it exceeed this limit in PPS
unsigned int ban_threshold_pps = 20000;

// We must ban IP of it exceed this limit for number of flows in any direction
unsigned int ban_threshold_flows = 3500;

// We must ban client if it exceed 1GBps
unsigned int ban_threshold_mbps = 1000;

// Number of lines for sending ben attack details to email
unsigned int ban_details_records_count = 500;


// log file
log4cpp::Category& logger = log4cpp::Category::getRoot();
string log_file_path = "/var/log/fastnetmon.log";
string attack_details_folder = "/var/log/fastnetmon_attacks";

/* Configuration block ends */

/* Our data structs */

// Enum with availible sort by field
enum sort_type { PACKETS, BYTES, FLOWS };

enum direction {
    INCOMING = 0,
    OUTGOING,
    INTERNAL,
    OTHER
};

typedef struct {
    uint64_t bytes;
    uint64_t packets;
    uint64_t flows;
} total_counter_element;

// We count total number of incoming/outgoing/internal and other traffic type packets/bytes
// And initilize by 0 all fields
total_counter_element total_counters[4];
total_counter_element total_speed_counters[4];

// Total amount of non parsed packets
uint64_t total_unparsed_packets = 0;

uint64_t incoming_total_flows_speed = 0;
uint64_t outgoing_total_flows_speed = 0;

typedef pair<uint32_t, uint32_t> subnet;

// main data structure for storing traffic and speed data for all our IPs
class map_element {
public:
    map_element() : in_bytes(0), out_bytes(0), in_packets(0), out_packets(0), tcp_in_packets(0), tcp_out_packets(0), tcp_in_bytes(0), tcp_out_bytes(0),
        udp_in_packets(0), udp_out_packets(0), udp_in_bytes(0), udp_out_bytes(0), in_flows(0), out_flows(0),
        icmp_in_packets(0), icmp_out_packets(0), icmp_in_bytes(0), icmp_out_bytes(0)
     {}
    uint64_t in_bytes;
    uint64_t out_bytes;
    uint64_t in_packets;
    uint64_t out_packets;
    
    // Additional data for correct attack protocol detection
    uint64_t tcp_in_packets;
    uint64_t tcp_out_packets;
    uint64_t tcp_in_bytes;
    uint64_t tcp_out_bytes;

    uint64_t udp_in_packets;
    uint64_t udp_out_packets;
    uint64_t udp_in_bytes;
    uint64_t udp_out_bytes;

    uint64_t icmp_in_packets;
    uint64_t icmp_out_packets;
    uint64_t icmp_in_bytes;
    uint64_t icmp_out_bytes;

    uint64_t in_flows;
    uint64_t out_flows;
};

// structure with attack details
class attack_details : public map_element {
    public:
    attack_details() :
        attack_protocol(0), attack_power(0), max_attack_power(0), average_in_bytes(0), average_out_bytes(0), average_in_packets(0), average_out_packets(0), average_in_flows(0), average_out_flows(0) {
    }    
    direction attack_direction;
    // first attackpower detected
    uint64_t attack_power;
    // max attack power
    uint64_t max_attack_power;
    unsigned int attack_protocol;

    // Average counters
    uint64_t average_in_bytes;
    uint64_t average_out_bytes;
    uint64_t average_in_packets;
    uint64_t average_out_packets;
    uint64_t average_in_flows;
    uint64_t average_out_flows;

    // time when we but this user
    time_t   ban_timestamp;
    int      ban_time; // seconds of the ban
};

typedef attack_details banlist_item;


// struct for save per direction and per protocol details for flow
typedef struct {
    uint64_t bytes;
    uint64_t packets;
    // will be used for Garbage Collection
    time_t   last_update_time;
} conntrack_key_struct;

typedef uint64_t packed_session;
// Main mega structure for storing conntracks
// We should use class instead struct for correct std::map allocation
typedef std::map<packed_session, conntrack_key_struct> contrack_map_type;

class conntrack_main_struct {
public:
    contrack_map_type in_tcp;
    contrack_map_type in_udp;
    contrack_map_type in_icmp;
    contrack_map_type in_other;

    contrack_map_type out_tcp;
    contrack_map_type out_udp;
    contrack_map_type out_icmp;
    contrack_map_type out_other;
};

typedef std::map <uint32_t, map_element> map_for_counters;
typedef vector<map_element> vector_of_counters;

typedef std::map <unsigned long int, vector_of_counters> map_of_vector_counters;

map_of_vector_counters SubnetVectorMap;

// Flow tracking structures
typedef vector<conntrack_main_struct> vector_of_flow_counters;
typedef std::map <unsigned long int, vector_of_flow_counters> map_of_vector_counters_for_flow;
map_of_vector_counters_for_flow SubnetVectorMapFlow;

class packed_conntrack_hash {
public:
    packed_conntrack_hash() : opposite_ip(0), src_port(0), dst_port(0) { } 
    // src or dst IP 
    uint32_t opposite_ip;
    uint16_t src_port;
    uint16_t dst_port;
};


// data structure for storing data in Vector
typedef pair<uint32_t, map_element> pair_of_map_elements;

/* End of our data structs */

boost::mutex data_counters_mutex;
boost::mutex speed_counters_mutex;
boost::mutex total_counters_mutex;

boost::mutex ban_list_details_mutex;

boost::mutex ban_list_mutex;
boost::mutex flow_counter;

#ifdef REDIS
redisContext *redis_context = NULL;
#endif

#ifdef PCAP
// pcap handler, we want it as global variable beacuse it used in singnal handler
pcap_t* descr = NULL;
#endif

#ifdef PF_RING
struct thread_stats {
    u_int64_t __padding_0[8];

    u_int64_t numPkts;
    u_int64_t numBytes;

    pfring *ring;
    pthread_t pd_thread;
    int core_affinity;

    volatile u_int64_t do_shutdown;

    u_int64_t __padding_1[3];
};

struct thread_stats *threads;
pfring* pf_ring_descr = NULL;
#endif

// map for flows
map<uint64_t, int> FlowCounter;

// Struct for string speed per IP
map_for_counters SpeedCounter;

// Struct for storing average speed per IP for specified interval 
map_for_counters SpeedCounterAverage;

#ifdef GEOIP
map_for_counters GeoIpCounter;
#endif

// In ddos info we store attack power and direction
map<uint32_t, banlist_item> ban_list;
map<uint32_t, vector<simple_packet> > ban_list_details;

// Standard shift for type DLT_EN10MB, Ethernet
unsigned int DATA_SHIFT_VALUE = 14;

vector<subnet> our_networks;
vector<subnet> whitelist_networks;

// Ban enable/disable flag
bool we_do_real_ban = true;

// Prototypes
#ifdef HWFILTER_LOCKING
void block_all_traffic_with_82599_hardware_filtering(string client_ip_as_string);
#endif

string get_pcap_stats();
string get_pf_ring_stats();
bool zc_main_loop(const char* device);
unsigned int get_max_used_protocol(uint64_t tcp, uint64_t udp, uint64_t icmp);
string get_printable_protocol_name(unsigned int protocol);
void print_attack_details_to_file(string details, string client_ip_as_string,  attack_details current_attack);
bool folder_exists(string path);
string print_time_t_in_fastnetmon_format(time_t current_time);
string print_ban_thresholds();
bool load_configuration_file();
std::string print_flow_tracking_for_ip(conntrack_main_struct& conntrack_element, string client_ip);
void convert_integer_to_conntrack_hash_struct(packed_session* packed_connection_data, packed_conntrack_hash* unpacked_data);
uint64_t convert_conntrack_hash_struct_to_integer(packed_conntrack_hash* struct_value);
int timeval_subtract (struct timeval * result, struct timeval * x,  struct timeval * y);
bool pf_ring_main_loop_multi_channel(const char* dev);
void* pf_ring_packet_consumer_thread(void* _id);
bool is_cidr_subnet(const char* subnet);
uint64_t MurmurHash64A (const void * key, int len, uint64_t seed);
void cleanup_ban_list();
string print_tcp_flags(uint8_t flag_value);
int extract_bit_value(uint8_t num, int bit);
string get_attack_description(uint32_t client_ip, attack_details& current_attack);
uint64_t convert_speed_to_mbps(uint64_t speed_in_bps);
void send_attack_details(uint32_t client_ip, attack_details current_attack_details);
string convert_timeval_to_date(struct timeval tv);
void free_up_all_resources();
void main_packet_process_task();
unsigned int get_cidr_mask_from_network_as_string(string network_cidr_format);
string print_ddos_attack_details();
void execute_ip_ban(uint32_t client_ip, map_element new_speed_element, uint64_t in_pps, uint64_t out_pps, uint64_t in_bps, uint64_t out_bps, uint64_t in_flows, uint64_t out_flows, string flow_attack_details);
direction get_packet_direction(uint32_t src_ip, uint32_t dst_ip, unsigned long& subnet);
void recalculate_speed();
std::string print_channel_speed(string traffic_type, direction packet_direction);
void process_packet(simple_packet& current_packet);
void copy_networks_from_string_form_to_binary(vector<string> networks_list_as_string, vector<subnet>& our_networks);

bool file_exists(string path);
void traffic_draw_programm();
void pcap_main_loop(const char* dev);
bool pf_ring_main_loop(const char* dev);
void parse_packet(u_char *user, struct pcap_pkthdr *packethdr, const u_char *packetptr);
void ulog_main_loop();
void signal_handler(int signal_number);
uint32_t convert_cidr_to_binary_netmask(unsigned int cidr);

/* Class for custom comparison fields by different fields */
class TrafficComparatorClass {
    private:
        sort_type sort_field;
        direction sort_direction;
    public:    
        TrafficComparatorClass(direction sort_direction, sort_type sort_field) {
            this->sort_field = sort_field;
            this->sort_direction = sort_direction;
        }

        bool operator()(pair_of_map_elements a, pair_of_map_elements b) {
            if (sort_field == FLOWS) {
                if (sort_direction == INCOMING) {
                    return a.second.in_flows > b.second.in_flows;
                } else if (sort_direction == OUTGOING) {
                    return a.second.out_flows > b.second.out_flows;
                } else {
                    return false;
                }
            } else if (sort_field == PACKETS) {
                if (sort_direction == INCOMING) {
                    return a.second.in_packets > b.second.in_packets; 
                } else if (sort_direction == OUTGOING) {
                    return a.second.out_packets > b.second.out_packets;
                } else {
                    return false;
                }
            } else if (sort_field == BYTES) {
                if (sort_direction == INCOMING) {
                    return a.second.in_bytes > b.second.in_bytes;
                } else if (sort_direction == OUTGOING) {
                    return a.second.out_bytes > b.second.out_bytes;
                } else {
                    return false;
                }    
            } else {
                return false;
            }
        }
};

string get_direction_name(direction direction_value) {
    string direction_name; 

    switch (direction_value) {
        case INCOMING: direction_name = "incoming"; break;
        case OUTGOING: direction_name = "outgoing"; break;
        case INTERNAL: direction_name = "internal"; break;
        case OTHER:    direction_name = "other";    break;
        default:       direction_name = "unknown";  break;
    }   

    return direction_name;
}

uint32_t convert_ip_as_string_to_uint(string ip) {
    struct in_addr ip_addr;
    inet_aton(ip.c_str(), &ip_addr);

    // in network byte order
    return ip_addr.s_addr;
}

string convert_ip_as_uint_to_string(uint32_t ip_as_integer) {
    struct in_addr ip_addr;
    ip_addr.s_addr = ip_as_integer;
    return (string)inet_ntoa(ip_addr);
}

// convert integer to string
string convert_int_to_string(int value) {
    std::stringstream out;
    out << value;

    return out.str();
}

// convert string to integer
int convert_string_to_integer(string line) {
    return atoi(line.c_str());
}

// exec command in shell
vector<string> exec(string cmd) {
    vector<string> output_list;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return output_list;

    char buffer[256];
    std::string result = "";
    while(!feof(pipe)) {
        if(fgets(buffer, 256, pipe) != NULL) {
            size_t newbuflen = strlen(buffer);
            
            // remove newline at the end
            if (buffer[newbuflen - 1] == '\n') {
                buffer[newbuflen - 1] = '\0';
            }

            output_list.push_back(buffer);
        }
    }

    pclose(pipe);
    return output_list;
}

// exec command and pass data to it stdin
bool exec_with_stdin_params(string cmd, string params) {
    FILE* pipe = popen(cmd.c_str(), "w");
    if (!pipe) {
        logger<<log4cpp::Priority::ERROR<<"Can't execute programm "<<cmd<<" error code: "<<errno<<" error text: "<<strerror(errno);
        return false;
    }

    if (fputs(params.c_str(), pipe)) {
        fclose(pipe);
        return true;
    } else {
        logger<<log4cpp::Priority::ERROR<<"Can't pass data to stdin of programm "<<cmd;
        fclose(pipe);
        return false;
    }
}

#ifdef GEOIP
bool geoip_init() {
    // load GeoIP ASN database to memory
    geo_ip = GeoIP_open("/root/fastnetmon/GeoIPASNum.dat", GEOIP_MEMORY_CACHE);

    if (geo_ip == NULL) {
        return false;
    } else {
        return true;
    }
}
#endif

#ifdef REDIS
bool redis_init_connection() {
    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    redis_context = redisConnectWithTimeout(redis_host.c_str(), redis_port, timeout);
    if (redis_context->err) {
        logger<<log4cpp::Priority::INFO<<"Connection error:"<<redis_context->errstr;
        return false;
    }

    // We should check connection with ping because redis do not check connection
    redisReply* reply = (redisReply*)redisCommand(redis_context, "PING");
    if (reply) {
        freeReplyObject(reply);
    } else {
        return false;
    }

    return true;
}

void update_traffic_in_redis(uint32_t ip, unsigned int traffic_bytes, direction my_direction) {
    string ip_as_string = convert_ip_as_uint_to_string(ip);
    redisReply *reply;

    if (!redis_context) {
        logger<< log4cpp::Priority::INFO<<"Please initialize Redis handle";
        return;
    }

    string key_name = ip_as_string + "_" + get_direction_name(my_direction);
    reply = (redisReply *)redisCommand(redis_context, "INCRBY %s %s", key_name.c_str(), convert_int_to_string(traffic_bytes).c_str());

    // If we store data correctly ...
    if (!reply) {
        logger.error("Can't increment traffic in redis error_code: %d error_string: %s", redis_context->err, redis_context->errstr);
   
        // Handle redis server restart corectly
        if (redis_context->err == 1 or redis_context->err == 3) {
            // Connection refused            
            redis_init_connection();
        }
    } else {
        freeReplyObject(reply); 
    }
}
#endif

string draw_table(map_for_counters& my_map_packets, direction data_direction, bool do_redis_update, sort_type sort_item) {
    std::vector<pair_of_map_elements> vector_for_sort;

    stringstream output_buffer;

    // Preallocate memory for sort vector
    vector_for_sort.reserve(my_map_packets.size());

    for( map_for_counters::iterator ii = my_map_packets.begin(); ii != my_map_packets.end(); ++ii) {
        // store all elements into vector for sorting
        vector_for_sort.push_back( make_pair((*ii).first, (*ii).second) );
    } 
 
    if (data_direction == INCOMING or data_direction == OUTGOING) {
        std::sort( vector_for_sort.begin(), vector_for_sort.end(), TrafficComparatorClass(data_direction, sort_item));
    } else {
        logger<< log4cpp::Priority::INFO<<"Unexpected bahaviour on sort function";
    }

    unsigned int element_number = 0;
    for( vector<pair_of_map_elements>::iterator ii=vector_for_sort.begin(); ii!=vector_for_sort.end(); ++ii) {
        uint32_t client_ip = (*ii).first;
        string client_ip_as_string = convert_ip_as_uint_to_string((*ii).first);

        uint64_t pps = 0; 
        uint64_t bps = 0;
        uint64_t flows = 0;

        uint64_t pps_average = 0;
        uint64_t bps_average = 0;
        uint64_t flows_average = 0;  

        // TODO: replace map by vector iteration 
        map_element* current_average_speed_element = &SpeedCounterAverage[client_ip];
        map_element* current_speed_element         = &SpeedCounter[client_ip];
 
        // Create polymorphic pps, byte and flow counters
        if (data_direction == INCOMING) {
            pps   = current_speed_element->in_packets;
            bps   = current_speed_element->in_bytes;
            flows = current_speed_element->in_flows;
       
            pps_average   = current_average_speed_element->in_packets;
            bps_average   = current_average_speed_element->in_bytes;
            flows_average = current_average_speed_element->in_flows;
        } else if (data_direction == OUTGOING) {
            pps   = current_speed_element->out_packets;
            bps   = current_speed_element->out_bytes;
            flows = current_speed_element->out_flows;
    
            pps_average = current_average_speed_element->out_packets;
            bps_average = current_average_speed_element->out_bytes;
            flows_average = current_average_speed_element->out_flows;
        }    

        uint64_t mbps = convert_speed_to_mbps(bps);
        uint64_t mbps_average = convert_speed_to_mbps(bps_average);

        // Print first max_ips_in_list elements in list, we will show top 20 "huge" channel loaders
        if (element_number < max_ips_in_list) {
            string is_banned = ban_list.count(client_ip) > 0 ? " *banned* " : "";
            // We use setw for alignment
            output_buffer<<client_ip_as_string << "\t\t";

            if (print_average_traffic_counts) {
                output_buffer<<setw(6)<<pps   << "/" << pps_average   << " pps ";
                output_buffer<<setw(6)<<mbps  << "/" << mbps_average  << " mbps ";
                output_buffer<<setw(6)<<flows << "/" << flows_average << " flows ";
            } else {
                output_buffer<<setw(6)<<pps<<" pps ";
                output_buffer<<setw(6)<<mbps<<" mbps ";
                output_buffer<<setw(6)<<flows << " flows ";
            }

            output_buffer<< is_banned << endl;
        }  
   
#ifdef REDIS 
        if (redis_enabled && do_redis_update) {
            update_traffic_in_redis( (*ii).first, (*ii).second.in_packets, INCOMING);
            update_traffic_in_redis( (*ii).first, (*ii).second.out_packets, OUTGOING);
        }
#endif
        
        element_number++;
    }

    return output_buffer.str(); 
}

// check file existence
bool file_exists(string path) {
    FILE* check_file = fopen(path.c_str(), "r");
    if (check_file) {
        fclose(check_file);
        return true;
    } else {
        return false;
    }
}

// read whole file to vector
vector<string> read_file_to_vector(string file_name) {
    vector<string> data;
    string line;

    ifstream reading_file;

    reading_file.open(file_name.c_str(), std::ifstream::in);
    if (reading_file.is_open()) {
        while ( getline(reading_file, line) ) {
            data.push_back(line); 
        }
    } else {
        logger<< log4cpp::Priority::ERROR <<"Can't open file: "<<file_name;
    }

    return data;
}

// Load configuration
bool load_configuration_file() {
    ifstream config_file (global_config_path.c_str());
    string line;

    map<string, std::string> configuration_map;
    
    if (!config_file.is_open()) {
        logger<< log4cpp::Priority::ERROR<<"Can't open config file";
        return false;
    }

    while ( getline(config_file, line) ) {
        vector<string> parsed_config; 
        boost::split( parsed_config, line, boost::is_any_of(" ="), boost::token_compress_on );

        if (parsed_config.size() == 2) {
            configuration_map[ parsed_config[0] ] = parsed_config[1];
        } else {
            logger<< log4cpp::Priority::ERROR<<"Can't parse config line: "<<line;
        }
    }

    if (configuration_map.count("enable_pf_ring_zc_mode")) {
        if (configuration_map["enable_pf_ring_zc_mode"] == "on") {
            pf_ring_zc_api_mode = true;
        } else {
            pf_ring_zc_api_mode = false;
        }
    }

    if (configuration_map.count("enable_connection_tracking")) {
        if (configuration_map["enable_connection_tracking"] == "on") {
            enable_conection_tracking = true;
        } else {
            enable_conection_tracking = false;
        }
    }

    if (configuration_map.count("ban_time") != 0) {
        standard_ban_time = convert_string_to_integer(configuration_map["ban_time"]);
    }

    if (configuration_map.count("average_calculation_time") != 0) {
        average_calculation_amount = convert_string_to_integer(configuration_map["average_calculation_time"]);
    }

    if (configuration_map.count("threshold_pps") != 0) {
        ban_threshold_pps = convert_string_to_integer( configuration_map[ "threshold_pps" ] );
    }

    if (configuration_map.count("threshold_mbps") != 0) {
        ban_threshold_mbps = convert_string_to_integer(  configuration_map[ "threshold_mbps" ] );
    }

    if (configuration_map.count("threshold_flows") != 0) {
        ban_threshold_flows = convert_string_to_integer(  configuration_map[ "threshold_flows" ] );
    }

    if (configuration_map.count("enable_ban") != 0) {
        if (configuration_map["enable_ban"] == "on") {
            we_do_real_ban = true;
        } else {
            we_do_real_ban = false;
        }
    }

    if (configuration_map.count("sflow") != 0) {
        if (configuration_map[ "sflow" ] == "on") {
            enable_sflow_collection = true;
        } else {
            enable_sflow_collection = false;
        }
    }

    if (configuration_map.count("mirror") != 0) { 
        if (configuration_map["mirror"] == "on") {
            enable_data_collection_from_mirror = true;
        } else {
            enable_data_collection_from_mirror = false;
        }
    }

    if (configuration_map.count("ban_for_pps") != 0) {
        if (configuration_map["ban_for_pps"] == "on") {
            enable_ban_for_pps = true;
        } else {
            enable_ban_for_pps = false;
        }
    }

    if (configuration_map.count("ban_for_bandwidth") != 0) { 
        if (configuration_map["ban_for_bandwidth"] == "on") {
            enable_ban_for_bandwidth = true;
        } else {
            enable_ban_for_bandwidth = false;
        }    
    }    

    if (configuration_map.count("ban_for_flows") != 0) { 
        if (configuration_map["ban_for_flows"] == "on") {
            enable_ban_for_flows_per_second = true;
        } else {
            enable_ban_for_flows_per_second = false;
        }    
    }    

#ifdef REDIS
    if (configuration_map.count("redis_port") != 0) { 
        redis_port = convert_string_to_integer(configuration_map[ "redis_port" ] );
    }

    if (configuration_map.count("redis_host") != 0) {
        redis_host = configuration_map[ "redis_host" ];
    }

    if (configuration_map.count("redis_enabled") != 0) {
        if (configuration_map[ "redis_enabled" ] == "yes") {
            redis_enabled = true;
        } else {
            redis_enabled = false;
        } 
    }
#endif

    if (configuration_map.count("ban_details_records_count") != 0 ) {
        ban_details_records_count = convert_string_to_integer( configuration_map[ "ban_details_records_count" ]);
    }

    if (configuration_map.count("check_period") != 0) {
        check_period = convert_string_to_integer( configuration_map[ "check_period" ]);
    }

    if (configuration_map.count("sort_parameter") != 0) {
        sort_parameter = configuration_map[ "sort_parameter" ];
    }

    if (configuration_map.count("interfaces") != 0) {
        work_on_interfaces = configuration_map[ "interfaces" ];

        // We should check all interfaces and check zc flag for all
        if (work_on_interfaces.find("zc:") != string::npos) {
            we_use_pf_ring_in_kernel_parser = false;
            logger<< log4cpp::Priority::INFO<<"We detect run in PF_RING Zero Copy or DNA mode and we enable packet parser!";
        }
    }

    if (configuration_map.count("max_ips_in_list") != 0) {
        max_ips_in_list = convert_string_to_integer( configuration_map[ "max_ips_in_list" ]);
    }

    if (configuration_map.count("notify_script_path") != 0 ) {
        notify_script_path = configuration_map[ "notify_script_path" ];
    }

    return true;
}

/* Enable core dumps for simplify debug tasks */
void enable_core_dumps() {
    struct rlimit rlim;

    int result = getrlimit(RLIMIT_CORE, &rlim);

    if (result) {
        logger<< log4cpp::Priority::ERROR<<"Can't get current rlimit for RLIMIT_CORE";
        return;
    } else {
        rlim.rlim_cur = rlim.rlim_max;
        setrlimit(RLIMIT_CORE, &rlim);
    }
}

void subnet_vectors_allocator(prefix_t* prefix, void* data) {
    // Network byte order
    uint32_t subnet_as_integer = prefix->add.sin.s_addr;

    u_short bitlen = prefix->bitlen;
    int network_size_in_ips = pow(2, 32-bitlen);
    //logger<< log4cpp::Priority::INFO<<"Subnet: "<<prefix->add.sin.s_addr<<" network size: "<<network_size_in_ips;
    logger<< log4cpp::Priority::INFO<<"I will allocate "<<network_size_in_ips<<" records for subnet "<<subnet_as_integer;

    // Initialize map element
    SubnetVectorMap[subnet_as_integer] = vector_of_counters(network_size_in_ips);

    // Zeroify all vector elements
    map_element zero_map_element;
    memset(&zero_map_element, 0, sizeof(zero_map_element));
    std::fill(SubnetVectorMap[subnet_as_integer].begin(), SubnetVectorMap[subnet_as_integer].end(), zero_map_element);

    // Initilize map element
    SubnetVectorMapFlow[subnet_as_integer] = vector_of_flow_counters(network_size_in_ips); 

    // On creating it initilizes by zeros
    conntrack_main_struct zero_conntrack_main_struct;
    std::fill(SubnetVectorMapFlow[subnet_as_integer].begin(), SubnetVectorMapFlow[subnet_as_integer].end(), zero_conntrack_main_struct);
}

void zeroify_all_counters() {
    map_element zero_map_element;
    memset(&zero_map_element, 0, sizeof(zero_map_element));

    for (map_of_vector_counters::iterator itr = SubnetVectorMap.begin(); itr != SubnetVectorMap.end(); itr++) {
        //logger<< log4cpp::Priority::INFO<<"Zeroify "<<itr->first;
        std::fill(itr->second.begin(), itr->second.end(), zero_map_element); 
    }
}

void zeroify_all_flow_counters() {
    // On creating it initilizes by zeros
    conntrack_main_struct zero_conntrack_main_struct;

    // Iterate over map
    for (map_of_vector_counters_for_flow::iterator itr = SubnetVectorMapFlow.begin(); itr != SubnetVectorMapFlow.end(); itr++) {
        // Iterate over vector
        for (vector_of_flow_counters::iterator vector_iterator = itr->second.begin(); vector_iterator != itr->second.end(); vector_iterator++) {
            // TODO: rewrite this monkey code
            vector_iterator->in_tcp.clear();
            vector_iterator->in_udp.clear();
            vector_iterator->in_icmp.clear();
            vector_iterator->in_other.clear();

            vector_iterator->out_tcp.clear();
            vector_iterator->out_udp.clear();
            vector_iterator->out_icmp.clear();
            vector_iterator->out_other.clear();
        }
    }
}

bool load_our_networks_list() {
    if (file_exists("/etc/networks_whitelist")) {
        vector<string> network_list_from_config = read_file_to_vector("/etc/networks_whitelist");

        for( vector<string>::iterator ii=network_list_from_config.begin(); ii!=network_list_from_config.end(); ++ii) {
            if (ii->length() > 0 && is_cidr_subnet(ii->c_str())) {
                make_and_lookup(whitelist_tree, const_cast<char*>(ii->c_str()));
            } else {
                logger<<log4cpp::Priority::ERROR<<"Can't parse line from whitelist: "<<*ii;
            }
        }

        logger<<log4cpp::Priority::INFO<<"We loaded "<<network_list_from_config.size()<< " networks from whitelist file";
    }
 
    vector<string> networks_list_as_string;
    // We can bould "our subnets" automatically here 
    if (file_exists("/proc/vz/version")) {
        logger<< log4cpp::Priority::INFO<<"We found OpenVZ";
        // Add /32 CIDR mask for every IP here
        vector<string> openvz_ips = read_file_to_vector("/proc/vz/veip");
        for( vector<string>::iterator ii=openvz_ips.begin(); ii!=openvz_ips.end(); ++ii) {
            // skip IPv6 addresses
            if (strstr(ii->c_str(), ":") != NULL) {
                continue;
            }

            // skip header
            if (strstr(ii->c_str(), "Version") != NULL) {
                continue;
            }

            vector<string> subnet_as_string; 
            split( subnet_as_string, *ii, boost::is_any_of(" "), boost::token_compress_on );
 
            string openvz_subnet = subnet_as_string[1] + "/32";
            networks_list_as_string.push_back(openvz_subnet);
        }

        logger<<log4cpp::Priority::INFO<<"We loaded "<<networks_list_as_string.size()<< " networks from /proc/vz/version";
    } 

    if (file_exists("/etc/networks_list")) { 
        vector<string> network_list_from_config = read_file_to_vector("/etc/networks_list");
        networks_list_as_string.insert(networks_list_as_string.end(), network_list_from_config.begin(), network_list_from_config.end());

        logger<<log4cpp::Priority::INFO<<"We loaded "<<network_list_from_config.size()<< " networks from networks file";
    }

    // Some consistency checks
    assert( convert_ip_as_string_to_uint("255.255.255.0")   == convert_cidr_to_binary_netmask(24) );
    assert( convert_ip_as_string_to_uint("255.255.255.255") == convert_cidr_to_binary_netmask(32) );

    for( vector<string>::iterator ii=networks_list_as_string.begin(); ii!=networks_list_as_string.end(); ++ii) { 
        if (ii->length() > 0 && is_cidr_subnet(ii->c_str())) { 
            unsigned int cidr_mask = get_cidr_mask_from_network_as_string(*ii);
            total_number_of_hosts_in_our_networks += pow(2, 32-cidr_mask);

            make_and_lookup(lookup_tree, const_cast<char*>(ii->c_str()));
        } else {
            logger<<log4cpp::Priority::ERROR<<"Can't parse line from subnet list: "<<*ii;
        }
    }    

    /* Preallocate data structures */

    patricia_process (lookup_tree, (void_fn_t)subnet_vectors_allocator);

    logger<<log4cpp::Priority::INFO<<"We start total zerofication of counters";
    zeroify_all_counters();
    logger<<log4cpp::Priority::INFO<<"We finished it";

    logger<<log4cpp::Priority::INFO<<"We loaded "<<networks_list_as_string.size()<<" subnets to our in-memory list of networks";
    logger<<log4cpp::Priority::INFO<<"Total number of monitored hosts (total size of all networks): "
        <<total_number_of_hosts_in_our_networks;

    return true;
}

// extract 24 from 192.168.1.1/24
unsigned int get_cidr_mask_from_network_as_string(string network_cidr_format) {
    vector<string> subnet_as_string; 
    split( subnet_as_string, network_cidr_format, boost::is_any_of("/"), boost::token_compress_on );

    if (subnet_as_string.size() != 2) {
        return 0;
    }

    return convert_string_to_integer(subnet_as_string[1]);
}

void copy_networks_from_string_form_to_binary(vector<string> networks_list_as_string, vector<subnet>& our_networks ) {
    for( vector<string>::iterator ii=networks_list_as_string.begin(); ii!=networks_list_as_string.end(); ++ii) {
        vector<string> subnet_as_string; 
        split( subnet_as_string, *ii, boost::is_any_of("/"), boost::token_compress_on );
        unsigned int cidr = convert_string_to_integer(subnet_as_string[1]);

        uint32_t subnet_as_int  = convert_ip_as_string_to_uint(subnet_as_string[0]);
        uint32_t netmask_as_int = convert_cidr_to_binary_netmask(cidr);

        subnet current_subnet = std::make_pair(subnet_as_int, netmask_as_int);

        our_networks.push_back(current_subnet);
    }  
} 

uint32_t convert_cidr_to_binary_netmask(unsigned int cidr) {
    uint32_t binary_netmask = 0xFFFFFFFF; 
    binary_netmask = binary_netmask << ( 32 - cidr );
    // htonl from host byte order to network
    // ntohl from network byte order to host

    // We need network byte order at output 
    return htonl(binary_netmask);
}

string get_printable_protocol_name(unsigned int protocol) {
    string proto_name;

    switch (protocol) {
        case IPPROTO_TCP:
            proto_name = "tcp";
            break;
        case IPPROTO_UDP:
            proto_name = "udp";
            break;
        case IPPROTO_ICMP:
            proto_name = "icmp";
            break;
        default:
            proto_name = "unknown";
            break;
    } 

    return proto_name;
}

string print_simple_packet(simple_packet packet) {
    std::stringstream buffer;

    buffer<<convert_timeval_to_date(packet.ts)<<" ";

    buffer
        <<convert_ip_as_uint_to_string(packet.src_ip)<<":"<<packet.source_port
        <<" > "
        <<convert_ip_as_uint_to_string(packet.dst_ip)<<":"<<packet.destination_port
        <<" protocol: "<<get_printable_protocol_name(packet.protocol);
   
    // Print flags only for TCP 
    if (packet.protocol == IPPROTO_TCP) { 
        buffer<<" flags: "<<print_tcp_flags(packet.flags);
    }

    buffer<<" size: "<<packet.length<<" bytes"<<"\n";
    
    return buffer.str();
}

void parse_packet_pf_ring(const struct pfring_pkthdr *h, const u_char *p, const u_char *user_bytes) {
    // Описание всех полей: http://www.ntop.org/pfring_api/structpkt__parsing__info.html
    simple_packet packet;

    if (!pf_ring_zc_api_mode) {
        if (!we_use_pf_ring_in_kernel_parser) {
            // In ZC (zc:eth0) mode you should manually add packet parsing here
            // Because it disabled by default: "parsing already disabled in zero-copy"
            // http://www.ntop.org/pfring_api/pfring_8h.html 
            // Parse up to L3, no timestamp, no hashing
            // 1 - add timestamp, 0 - disable hash

            // We should zeroify packet header because PFRING ZC did not do this!
            memset((void*)&h->extended_hdr.parsed_pkt, 0, sizeof(h->extended_hdr.parsed_pkt));
            pfring_parse_pkt((u_char*)p, (struct pfring_pkthdr*)h, 4, 1, 0);
        }
    }

    if (do_unpack_l2tp_over_ip) {
        // 2014-12-08 13:36:53,537 [INFO] [00:1F:12:84:E2:E7 -> 90:E2:BA:49:85:C8] [IPv4][5.254.105.102:0 -> 159.253.17.251:0] [l3_proto=115][hash=2784721876][tos=32][tcp_seq_num=0] [caplen=128][len=873][parsed_header_len=0][eth_offset=-14][l3_offset=14][l4_offset=34][payload_offset=0]
        // L2TP has an proto number 115
        if (h->extended_hdr.parsed_pkt.l3_proto == 115) {
            // pfring_parse_pkt expects that the hdr memory is either zeroed or contains valid values
            // for the current packet, in order to avoid parsing twice the same packet headers.
            struct pfring_pkthdr l2tp_header;
            memset(&l2tp_header, 0, sizeof(l2tp_header));

            int16_t l4_offset = h->extended_hdr.parsed_pkt.offset.l4_offset;

            // L2TP has two headers: L2TP and default L2-Specific Sublayer: every header for 4bytes
            int16_t l2tp_header_size = 8;
            l2tp_header.len = h->len - (l4_offset + l2tp_header_size);
            l2tp_header.caplen = h->caplen - (l4_offset + l2tp_header_size);

            const u_char *l2tp_tunnel_payload = p + l4_offset + l2tp_header_size;
            // 1 - add timestamp, 0 - disable hash
            pfring_parse_pkt((u_char*)l2tp_tunnel_payload, &l2tp_header, 4, 1, 0);

            // Copy data back
            // TODO: it's not fine solution and I should redesign this code
            memcpy((struct pfring_pkthdr*)h, &l2tp_header, sizeof(l2tp_header));

            // TODO: Global pfring_print_parsed_pkt can fail because we did not shift 'p' pointer

            // Uncomment this line for deep inspection of all packets
            /*
            char buffer[512];
            pfring_print_parsed_pkt(buffer, 512, l2tp_tunnel_payload, h);
            logger<<log4cpp::Priority::INFO<<buffer;
            */
        }    
    } 

    /* We handle only IPv4 */
    if (h->extended_hdr.parsed_pkt.ip_version == 4) {
        /* PF_RING stores data in host byte order but we use network byte order */
        packet.src_ip = htonl( h->extended_hdr.parsed_pkt.ip_src.v4 ); 
        packet.dst_ip = htonl( h->extended_hdr.parsed_pkt.ip_dst.v4 );

        packet.source_port      = h->extended_hdr.parsed_pkt.l4_src_port;
        packet.destination_port = h->extended_hdr.parsed_pkt.l4_dst_port;

        packet.length   = h->len;
        packet.protocol = h->extended_hdr.parsed_pkt.l3_proto;
        packet.ts       = h->ts;

        // Copy flags from PF_RING header to our pseudo header
        if (packet.protocol == IPPROTO_TCP) {
            packet.flags = h->extended_hdr.parsed_pkt.tcp.flags;
        } else {
            packet.flags = 0;
        } 

        process_packet(packet);
    } else {
        total_unparsed_packets++;
    }

    // Uncomment this line for deep inspection of all packets

    /*    
    char buffer[512];
    pfring_print_parsed_pkt(buffer, 512, p, h);
    logger<<log4cpp::Priority::INFO<<buffer;
    */
}

// We do not use this function now! It's buggy!
void parse_packet(u_char *user, struct pcap_pkthdr *packethdr, const u_char *packetptr) {
    struct ip* iphdr;
    struct tcphdr* tcphdr;
    struct udphdr* udphdr;

    struct ether_header *eptr;    /* net/ethernet.h */
    eptr = (struct ether_header* )packetptr;

    if ( ntohs(eptr->ether_type) ==  VLAN_ETHERTYPE ) {
        // It's tagged traffic we should sjoft for 4 bytes for getting the data
        packetptr += DATA_SHIFT_VALUE + VLAN_HDRLEN;
    } else if (ntohs(eptr->ether_type) == IP_ETHERTYPE) {
        // Skip the datalink layer header and get the IP header fields.
        packetptr += DATA_SHIFT_VALUE;
    } else if (ntohs(eptr->ether_type) == IP6_ETHERTYPE or ntohs(eptr->ether_type) == ARP_ETHERTYPE) {
        // we know about it but does't not care now
    } else  {
        // printf("Packet with non standard ethertype found: 0x%x\n", ntohs(eptr->ether_type));
    }

    iphdr = (struct ip*)packetptr;

    // src/dst UO is an in_addr, http://man7.org/linux/man-pages/man7/ip.7.html
    uint32_t src_ip = iphdr->ip_src.s_addr;
    uint32_t dst_ip = iphdr->ip_dst.s_addr;

    // The ntohs() function converts the unsigned short integer netshort from network byte order to host byte order
    unsigned int packet_length = ntohs(iphdr->ip_len); 

    simple_packet current_packet;

    // Advance to the transport layer header then parse and display
    // the fields based on the type of hearder: tcp, udp or icmp
    packetptr += 4*iphdr->ip_hl;
    switch (iphdr->ip_p) {
        case IPPROTO_TCP: 
            tcphdr = (struct tcphdr*)packetptr;
            current_packet.source_port = ntohs(tcphdr->source);
            current_packet.destination_port = ntohs(tcphdr->dest);
            break;
        case IPPROTO_UDP:
            udphdr = (struct udphdr*)packetptr;
            current_packet.source_port = ntohs(udphdr->source);
            current_packet.destination_port = ntohs(udphdr->dest);
            break;
        case IPPROTO_ICMP:
            // there are no port for ICMP
            current_packet.source_port = 0;
            current_packet.destination_port = 0;
            break;
    }

    current_packet.protocol = iphdr->ip_p;
    current_packet.src_ip = src_ip;
    current_packet.dst_ip = dst_ip;
    current_packet.length = packet_length;
    
    // Do packet processing
    process_packet(current_packet);
}

/* Process simple unified packet */
void process_packet(simple_packet& current_packet) { 
    // Packets dump is very useful for bug hunting
    if (DEBUG_DUMP_ALL_PACKETS) {
        logger<< log4cpp::Priority::INFO<<"Dump: "<<print_simple_packet(current_packet);
    }

    // Subnet for found IPs
    unsigned long subnet = 0;
    direction packet_direction = get_packet_direction(current_packet.src_ip, current_packet.dst_ip, subnet);

    uint32_t subnet_in_host_byte_order = 0;
    // We operate in host bytes order and need to convert subnet
    if (subnet != 0) {
        subnet_in_host_byte_order = ntohl(subnet);
    }

    // Try to find map key for this subnet
    map_of_vector_counters::iterator itr;

    if (packet_direction == OUTGOING or packet_direction == INCOMING) {
        itr = SubnetVectorMap.find(subnet);

        if (itr == SubnetVectorMap.end()) {
            logger<< log4cpp::Priority::ERROR<<"Can't find vector address in subnet map";
            return; 
        }
    }

    map_of_vector_counters_for_flow::iterator itr_flow;

    if (enable_conection_tracking) {
        if (packet_direction == OUTGOING or packet_direction == INCOMING) {
            itr_flow = SubnetVectorMapFlow.find(subnet);

            if (itr_flow == SubnetVectorMapFlow.end()) {
                logger<< log4cpp::Priority::ERROR<<"Can't find vector address in subnet flow map";
                return;
            }
        }
    }

    uint32_t sampled_number_of_packets = current_packet.sample_ratio;
    uint32_t sampled_number_of_bytes = current_packet.length * current_packet.sample_ratio;

    __sync_fetch_and_add(&total_counters[packet_direction].packets, sampled_number_of_packets);
    __sync_fetch_and_add(&total_counters[packet_direction].bytes,   sampled_number_of_bytes);
    
    // Incerementi main and per protocol packet counters
    if (packet_direction == OUTGOING) {
        uint32_t shift_in_vector = ntohl(current_packet.src_ip) - subnet_in_host_byte_order;
        map_element* current_element = &itr->second[shift_in_vector];

        // Main packet/bytes counter
        __sync_fetch_and_add(&current_element->out_packets, sampled_number_of_packets);
        __sync_fetch_and_add(&current_element->out_bytes,   sampled_number_of_bytes);

        conntrack_main_struct* current_element_flow = NULL;
        if (enable_conection_tracking) {
            current_element_flow = &itr_flow->second[shift_in_vector]; 
        }

        // Collect data when ban client
        if  (ban_list_details.size() > 0 && ban_list_details.count(current_packet.src_ip) > 0 &&
            ban_list_details[current_packet.src_ip].size() < ban_details_records_count) {

            ban_list_details_mutex.lock();
            ban_list_details[current_packet.src_ip].push_back(current_packet);
            ban_list_details_mutex.unlock();
        }

        uint64_t connection_tracking_hash = 0;

        if (enable_conection_tracking) {
            packed_conntrack_hash flow_tracking_structure;
            flow_tracking_structure.opposite_ip = current_packet.dst_ip;
            flow_tracking_structure.src_port = current_packet.source_port;
            flow_tracking_structure.dst_port = current_packet.destination_port;

            // convert this struct to 64 bit integer
            connection_tracking_hash = convert_conntrack_hash_struct_to_integer(&flow_tracking_structure);
        }

        if (current_packet.protocol == IPPROTO_TCP) {
            __sync_fetch_and_add(&current_element->tcp_out_packets, sampled_number_of_packets);
            __sync_fetch_and_add(&current_element->tcp_out_bytes,   sampled_number_of_bytes);    

            if (enable_conection_tracking) {
                flow_counter.lock();
                conntrack_key_struct* conntrack_key_struct_ptr = &current_element_flow->out_tcp[connection_tracking_hash];
 
                conntrack_key_struct_ptr->packets += sampled_number_of_packets;
                conntrack_key_struct_ptr->bytes   += sampled_number_of_bytes;

                flow_counter.unlock();
            }
        } else if (current_packet.protocol == IPPROTO_UDP) {    
            __sync_fetch_and_add(&current_element->udp_out_packets, sampled_number_of_packets);
            __sync_fetch_and_add(&current_element->udp_out_bytes,   sampled_number_of_bytes);

            if (enable_conection_tracking) {
                flow_counter.lock();
                conntrack_key_struct* conntrack_key_struct_ptr = &current_element_flow->out_udp[connection_tracking_hash];

                conntrack_key_struct_ptr->packets += sampled_number_of_packets;
                conntrack_key_struct_ptr->bytes   += sampled_number_of_bytes;
 
                flow_counter.unlock();
            }
        } else if (current_packet.protocol == IPPROTO_ICMP) {
            __sync_fetch_and_add(&current_element->icmp_out_packets, sampled_number_of_packets);
            __sync_fetch_and_add(&current_element->icmp_out_bytes,   sampled_number_of_bytes);
            // no flow tracking for icmp
        } else {

        } 

    } else if (packet_direction == INCOMING) {
        uint32_t shift_in_vector = ntohl(current_packet.dst_ip) - subnet_in_host_byte_order;
        map_element* current_element = &itr->second[shift_in_vector];
   
        // Main packet/bytes counter 
        __sync_fetch_and_add(&current_element->in_packets, sampled_number_of_packets);
        __sync_fetch_and_add(&current_element->in_bytes,   sampled_number_of_bytes);

        conntrack_main_struct* current_element_flow = NULL;
   
        if (enable_conection_tracking) {
            current_element_flow = &itr_flow->second[shift_in_vector];
        }
 
        uint64_t connection_tracking_hash = 0;
        if (enable_conection_tracking) {
            packed_conntrack_hash flow_tracking_structure;
            flow_tracking_structure.opposite_ip = current_packet.src_ip;
            flow_tracking_structure.src_port = current_packet.source_port;
            flow_tracking_structure.dst_port = current_packet.destination_port;

            // convert this struct to 64 bit integer
            connection_tracking_hash = convert_conntrack_hash_struct_to_integer(&flow_tracking_structure);
        }

        // Collect attack details
        if  (ban_list_details.size() > 0 && ban_list_details.count(current_packet.dst_ip) > 0 &&
            ban_list_details[current_packet.dst_ip].size() < ban_details_records_count) {

            ban_list_details_mutex.lock();
            ban_list_details[current_packet.dst_ip].push_back(current_packet);
            ban_list_details_mutex.unlock();
        }

        if (current_packet.protocol == IPPROTO_TCP) {
            __sync_fetch_and_add(&current_element->tcp_in_packets, sampled_number_of_packets);
            __sync_fetch_and_add(&current_element->tcp_in_bytes,   sampled_number_of_bytes);

            if (enable_conection_tracking) {
                flow_counter.lock();
                conntrack_key_struct* conntrack_key_struct_ptr = &current_element_flow->in_tcp[connection_tracking_hash];

                conntrack_key_struct_ptr->packets += sampled_number_of_packets;
                conntrack_key_struct_ptr->bytes   += sampled_number_of_bytes;

                flow_counter.unlock();
            }
        } else if (current_packet.protocol == IPPROTO_UDP) {
            __sync_fetch_and_add(&current_element->udp_in_packets, sampled_number_of_packets);
            __sync_fetch_and_add(&current_element->udp_in_bytes,   sampled_number_of_bytes);

            if (enable_conection_tracking) {
                flow_counter.lock();
                conntrack_key_struct* conntrack_key_struct_ptr = &current_element_flow->in_udp[connection_tracking_hash];

                conntrack_key_struct_ptr->packets += sampled_number_of_packets;
                conntrack_key_struct_ptr->bytes   += sampled_number_of_bytes;
                flow_counter.unlock();
            }
        } else if (current_packet.protocol == IPPROTO_ICMP) {
            __sync_fetch_and_add(&current_element->icmp_in_packets, sampled_number_of_packets);
            __sync_fetch_and_add(&current_element->icmp_in_bytes,   sampled_number_of_bytes);

             // no flow tarcking for icmp
        } else {
            // TBD
        }

    } else if (packet_direction == INTERNAL) {

    }
}

#ifdef GEOIP
unsigned int get_asn_for_ip(uint32_t ip) { 
    char* asn_raw = GeoIP_org_by_name(geo_ip, convert_ip_as_uint_to_string(remote_ip).c_str());
    uint32_t asn_number = 0;
   
    if (asn_raw == NULL) {
        asn_number = 0; 
    } else {
        // split string: AS1299 TeliaSonera International Carrier
        vector<string> asn_as_string;
        split( asn_as_string, asn_raw, boost::is_any_of(" "), boost::token_compress_on );

        // free up original string
        free(asn_raw);

        // extract raw number
        asn_number = convert_string_to_integer(asn_as_string[0].substr(2)); 
    }
 
    return asn_number;
}
#endif 

// void* void* data
// It's not an calculation thread, it's vizualization thread :)
void calculation_thread() {
    // we need wait one second for calculating speed by recalculate_speed

    //#include <sys/prctl.h>
    //prctl(PR_SET_NAME , "fastnetmon calc thread", 0, 0, 0);

    // Sleep for a half second for shift against calculatiuon thread
    boost::this_thread::sleep(boost::posix_time::milliseconds(500));

    while (1) {
        // Availible only from boost 1.54: boost::this_thread::sleep_for( boost::chrono::seconds(check_period) );
        boost::this_thread::sleep(boost::posix_time::seconds(check_period));
        traffic_draw_programm();
    }
}

void recalculate_speed_thread_handler() {
    while (1) {
        // recalculate data every one second
        // Availible only from boost 1.54: boost::this_thread::sleep_for( boost::chrono::seconds(1) );
        boost::this_thread::sleep(boost::posix_time::seconds(1));
        recalculate_speed();
    }
}

/* Calculate speed for all connnections */
void recalculate_speed() {
    //logger<< log4cpp::Priority::INFO<<"We run recalculate_speed";

    struct timeval start_calc_time;
    gettimeofday(&start_calc_time, NULL);

    double speed_calc_period = 1;
    time_t start_time;
    time(&start_time);

    // If we got 1+ seconds lag we should use new "delta" or skip this step
    double time_difference = difftime(start_time, last_call_of_traffic_recalculation);

    if (time_difference < 1) {
        // It could occur on programm start
        logger<< log4cpp::Priority::INFO<<"We skip one iteration of speed_calc because it runs so early!";        
        return;
    } else if (int(time_difference) == 1) {
        // All fine, we run on time
    } else {
        logger<< log4cpp::Priority::INFO<<"Time from last run of speed_recalc is soooo big, we got ugly lags: "<<time_difference;
        speed_calc_period = time_difference;
    }

    map_element zero_map_element;
    memset(&zero_map_element, 0, sizeof(zero_map_element));
   
    uint64_t incoming_total_flows = 0;
    uint64_t outgoing_total_flows = 0;
 
    for (map_of_vector_counters::iterator itr = SubnetVectorMap.begin(); itr != SubnetVectorMap.end(); ++itr) {
        for (vector_of_counters::iterator vector_itr = itr->second.begin(); vector_itr !=  itr->second.end(); ++vector_itr) {
            int current_index = vector_itr - itr->second.begin();
            
            // New element
            map_element new_speed_element;

            // convert to host order for math operations
            uint32_t subnet_ip = ntohl(itr->first);
            uint32_t client_ip_in_host_bytes_order = subnet_ip + current_index;

            // covnert to our standard network byte order
            uint32_t client_ip = htonl(client_ip_in_host_bytes_order); 
            
            new_speed_element.in_packets  = uint64_t((double)vector_itr->in_packets   / speed_calc_period);
            new_speed_element.out_packets = uint64_t((double)vector_itr->out_packets  / speed_calc_period);

            new_speed_element.in_bytes  = uint64_t((double)vector_itr->in_bytes  / speed_calc_period);
            new_speed_element.out_bytes = uint64_t((double)vector_itr->out_bytes / speed_calc_period);     

            // By protocol counters

            // TCP
            new_speed_element.tcp_in_packets  = uint64_t((double)vector_itr->tcp_in_packets   / speed_calc_period);
            new_speed_element.tcp_out_packets = uint64_t((double)vector_itr->tcp_out_packets  / speed_calc_period);

            new_speed_element.tcp_in_bytes  = uint64_t((double)vector_itr->tcp_in_bytes  / speed_calc_period);
            new_speed_element.tcp_out_bytes = uint64_t((double)vector_itr->tcp_out_bytes / speed_calc_period);    

            // UDP
            new_speed_element.udp_in_packets  = uint64_t((double)vector_itr->udp_in_packets   / speed_calc_period);
            new_speed_element.udp_out_packets = uint64_t((double)vector_itr->udp_out_packets  / speed_calc_period);

            new_speed_element.udp_in_bytes  = uint64_t((double)vector_itr->udp_in_bytes  / speed_calc_period);
            new_speed_element.udp_out_bytes = uint64_t((double)vector_itr->udp_out_bytes / speed_calc_period); 

            // ICMP
            new_speed_element.icmp_in_packets  = uint64_t((double)vector_itr->icmp_in_packets   / speed_calc_period);
            new_speed_element.icmp_out_packets = uint64_t((double)vector_itr->icmp_out_packets  / speed_calc_period);

            new_speed_element.icmp_in_bytes  = uint64_t((double)vector_itr->icmp_in_bytes  / speed_calc_period);
            new_speed_element.icmp_out_bytes = uint64_t((double)vector_itr->icmp_out_bytes / speed_calc_period);

            conntrack_main_struct* flow_counter_ptr = &SubnetVectorMapFlow[itr->first][current_index]; 

            // todo: optimize this operations!
            uint64_t total_out_flows =
                (uint64_t)flow_counter_ptr->out_tcp.size()  +
                (uint64_t)flow_counter_ptr->out_udp.size()  +
                (uint64_t)flow_counter_ptr->out_icmp.size() +
                (uint64_t)flow_counter_ptr->out_other.size();

            uint64_t total_in_flows =
                (uint64_t)flow_counter_ptr->in_tcp.size()  +
                (uint64_t)flow_counter_ptr->in_udp.size()  +
                (uint64_t)flow_counter_ptr->in_icmp.size() +
                (uint64_t)flow_counter_ptr->in_other.size();

            new_speed_element.out_flows = uint64_t((double)total_out_flows  / speed_calc_period);
            new_speed_element.in_flows  = uint64_t((double)total_in_flows   / speed_calc_period);

            // Increment global counter
            incoming_total_flows += new_speed_element.in_flows;
            outgoing_total_flows += new_speed_element.out_flows;

            /* Moving average recalculation */
            // http://en.wikipedia.org/wiki/Moving_average#Application_to_measuring_computer_performance 
            //double speed_calc_period = 1; 
            double exp_power = -speed_calc_period/average_calculation_amount;
            double exp_value = exp(exp_power);

            map_element* current_average_speed_element = &SpeedCounterAverage[client_ip]; 
 
            current_average_speed_element->in_bytes  = uint64_t(new_speed_element.in_bytes  + exp_value *
                ((double)current_average_speed_element->in_bytes - (double)new_speed_element.in_bytes));
            current_average_speed_element->out_bytes = uint64_t(new_speed_element.out_bytes + exp_value *
                ((double)current_average_speed_element->out_bytes - (double)new_speed_element.out_bytes)); 

            current_average_speed_element->in_packets  = uint64_t(new_speed_element.in_packets  + exp_value *
                ((double)current_average_speed_element->in_packets -  (double)new_speed_element.in_packets));
            current_average_speed_element->out_packets = uint64_t(new_speed_element.out_packets + exp_value *
                ((double)current_average_speed_element->out_packets - (double)new_speed_element.out_packets));

            current_average_speed_element->out_flows = uint64_t(new_speed_element.out_flows + exp_value *
                ((double)current_average_speed_element->out_flows -  (double)new_speed_element.out_flows));
            current_average_speed_element->in_flows = uint64_t(new_speed_element.in_flows + exp_value *
                ((double)current_average_speed_element->in_flows -  (double)new_speed_element.in_flows));

            uint64_t in_pps_average  = current_average_speed_element->in_packets;
            uint64_t out_pps_average = current_average_speed_element->out_packets;

            uint64_t in_bps_average  = current_average_speed_element->in_bytes;
            uint64_t out_bps_average = current_average_speed_element->out_bytes; 

            uint64_t in_flows_average  = current_average_speed_element->in_flows;
            uint64_t out_flows_average = current_average_speed_element->out_flows;

            /* Moving average recalculation end */

            // we detect overspeed by packets
            bool attack_detected_by_pps = false;
            bool attack_detected_by_bandwidth = false;
            bool attack_detected_by_flow = false;

            if (enable_ban_for_pps && (in_pps_average > ban_threshold_pps or out_pps_average > ban_threshold_pps)) {
                attack_detected_by_pps = true;
            }

            // we detect overspeed by bandwidth
            if (enable_ban_for_bandwidth && (convert_speed_to_mbps(in_bps_average) > ban_threshold_mbps or convert_speed_to_mbps(out_bps_average) > ban_threshold_mbps)) {
                attack_detected_by_bandwidth = true;
            }

            if (enable_ban_for_flows_per_second && (in_flows_average > ban_threshold_flows or out_flows_average > ban_threshold_flows)) {
                attack_detected_by_flow = true; 
            } 

            if (attack_detected_by_pps or attack_detected_by_bandwidth or attack_detected_by_flow) {
                string flow_attack_details = "";
                
                if (enable_conection_tracking) {
                    flow_attack_details = print_flow_tracking_for_ip(*flow_counter_ptr, convert_ip_as_uint_to_string(client_ip));
                }
        
                // TODO: we should pass type of ddos ban source (pps, flowd, bandwidth)!
                execute_ip_ban(client_ip, new_speed_element, in_pps_average, out_pps_average, in_bps_average, out_bps_average, in_flows_average, out_flows_average, flow_attack_details);
            }
    
            speed_counters_mutex.lock();
            //map_element* current_speed_element = &SpeedCounter[client_ip];
            //*current_speed_element = new_speed_element;
            SpeedCounter[client_ip] = new_speed_element;
            speed_counters_mutex.unlock();

            data_counters_mutex.lock();
            *vector_itr = zero_map_element;
            data_counters_mutex.unlock();
        } 
    }

    // Calculate global flow speed
    incoming_total_flows_speed = uint64_t((double)incoming_total_flows / (double)speed_calc_period);
    outgoing_total_flows_speed = uint64_t((double)outgoing_total_flows / (double)speed_calc_period);
    
    if (enable_conection_tracking) {
        // Clean Flow Counter
        flow_counter.lock();
        zeroify_all_flow_counters();
        flow_counter.unlock();
    }

    for (unsigned int index = 0; index < 4; index++) {
        total_speed_counters[index].bytes   = uint64_t((double)total_counters[index].bytes   / (double)speed_calc_period);
        total_speed_counters[index].packets = uint64_t((double)total_counters[index].packets / (double)speed_calc_period);

        // nullify data counters after speed calculation
        //total_counters_mutex.lock();
        total_counters[index].bytes = 0; 
        total_counters[index].packets = 0; 
        //total_counters_mutex.unlock();
    }    

    // Set time of previous startup 
    time(&last_call_of_traffic_recalculation);

    struct timeval finish_calc_time;
    gettimeofday(&finish_calc_time, NULL);

    timeval_subtract(&speed_calculation_time, &finish_calc_time, &start_calc_time);
}

void print_screen_contents_into_file(string screen_data_stats_param) {
    ofstream screen_data_file;
    screen_data_file.open("/tmp/fastnetmon.dat", ios::trunc);

    if (screen_data_file.is_open()) {
        screen_data_file<<screen_data_stats_param;
        screen_data_file.close();
    } else {
        logger<<log4cpp::Priority::ERROR<<"Can't print programm screen into file";
    }
}

void traffic_draw_programm() {
    stringstream output_buffer;
   
    //logger<<log4cpp::Priority::INFO<<"Draw table call";
 
    struct timeval start_calc_time;
    gettimeofday(&start_calc_time, NULL);

    sort_type sorter;
    if (sort_parameter == "packets") {
        sorter = PACKETS;
    } else if (sort_parameter == "bytes") {
        sorter = BYTES;
    } else if (sort_parameter == "flows") {
        sorter = FLOWS;
    } else {
        logger<< log4cpp::Priority::INFO<<"Unexpected sorter type: "<<sort_parameter;
        sorter = PACKETS;
    }

    output_buffer<<"FastNetMon v1.0 FastVPS Eesti OU (c) VPS and dedicated: http://FastVPS.host"<<"\n"
        <<"IPs ordered by: "<<sort_parameter<<"\n";

    output_buffer<<print_channel_speed("Incoming traffic", INCOMING)<<endl;
    output_buffer<<draw_table(SpeedCounter, INCOMING, true, sorter);
    
    output_buffer<<endl; 
    
    output_buffer<<print_channel_speed("Outgoing traffic", OUTGOING)<<endl;
    output_buffer<<draw_table(SpeedCounter, OUTGOING, false, sorter);

    output_buffer<<endl;

    output_buffer<<print_channel_speed("Internal traffic", INTERNAL)<<endl;

    output_buffer<<endl;

    output_buffer<<print_channel_speed("Other traffic", OTHER)<<endl;

    output_buffer<<endl;

#ifdef PCAP
    output_buffer<<get_pcap_stats();
#endif
    // Application statistics
    output_buffer<<"Screen updated in:\t\t"<< drawing_thread_execution_time.tv_sec<<" sec "<<drawing_thread_execution_time.tv_usec<<" microseconds\n";
    output_buffer<<"Traffic calculated in:\t\t"<< speed_calculation_time.tv_sec<<" sec "<<speed_calculation_time.tv_usec<<" microseconds\n";
    output_buffer<<"Total amount of not processed packets: "<<total_unparsed_packets<<"\n";

#ifdef PF_RING  
    output_buffer<<get_pf_ring_stats();
#endif

    // Print thresholds
    output_buffer<<"\n\n"<<print_ban_thresholds();

    if (!ban_list.empty()) {
        output_buffer<<endl<<"Ban list:"<<endl;  
        output_buffer<<print_ddos_attack_details();
    }

    screen_data_stats = output_buffer.str();

    // Print screen contents into file
    print_screen_contents_into_file(screen_data_stats);

    struct timeval end_calc_time;
    gettimeofday(&end_calc_time, NULL);

    timeval_subtract(&drawing_thread_execution_time, &end_calc_time, &start_calc_time);
}

// pretty print channel speed in pps and MBit
std::string print_channel_speed(string traffic_type, direction packet_direction) {
    uint64_t speed_in_pps = total_speed_counters[packet_direction].packets;
    uint64_t speed_in_bps = total_speed_counters[packet_direction].bytes;

    unsigned int number_of_tabs = 1; 
    // We need this for correct alignment of blocks
    if (traffic_type == "Other traffic") {
        number_of_tabs = 2;
    }
 
    std::stringstream stream;
    stream<<traffic_type;

    for (unsigned int i = 0; i < number_of_tabs; i ++ ) {
        stream<<"\t";
    }

    uint64_t speed_in_mbps = convert_speed_to_mbps(speed_in_bps);

    stream<<setw(6)<<speed_in_pps<<" pps "<<setw(6)<<speed_in_mbps<<" mbps";

    if (traffic_type ==  "Incoming traffic" or traffic_type ==  "Outgoing traffic") {
        if (packet_direction == INCOMING) {
            stream<<" "<<setw(6)<<incoming_total_flows_speed<<" flows";
        } else if (packet_direction == OUTGOING) {
            stream<<" "<<setw(6)<<outgoing_total_flows_speed<<" flows";
        }
    }
 
    return stream.str();
}    

uint64_t convert_speed_to_mbps(uint64_t speed_in_bps) {
    return uint64_t((double)speed_in_bps / 1024 / 1024 * 8);
}

void init_logging() {
    log4cpp::PatternLayout* layout = new log4cpp::PatternLayout(); 
    layout->setConversionPattern ("%d [%p] %m%n"); 

    log4cpp::Appender *appender = new log4cpp::FileAppender("default", log_file_path);
    appender->setLayout(layout);

    logger.setPriority(log4cpp::Priority::INFO);
    logger.addAppender(appender);
    logger.info("Logger initialized!");
}

bool folder_exists(string path) {
    if (access(path.c_str(), 0) == 0) {
        struct stat status;
        stat(path.c_str(), &status);

        if (status.st_mode & S_IFDIR) {
            return true;
        }
    }

    return false;
}

int main(int argc,char **argv) {
    lookup_tree = New_Patricia(32);
    whitelist_tree = New_Patricia(32);

    // nullify total counters
    for (int index = 0; index < 4; index++) {
        total_counters[index].bytes = 0; 
        total_counters[index].packets = 0;

        total_speed_counters[index].bytes = 0;
        total_speed_counters[index].packets = 0; 
    } 

    // enable core dumps
    enable_core_dumps();

    init_logging();

    /* Create folder for attack details */
    if (!folder_exists(attack_details_folder)) {
        int mkdir_result = mkdir(attack_details_folder.c_str(), S_IRWXU);

        if (mkdir_result != 0) {
            logger<<log4cpp::Priority::ERROR<<"Can't create folder for attack details: "<<attack_details_folder;
            exit(1);
        }
    }

    if (getenv("DUMP_ALL_PACKETS") != NULL) {
        DEBUG_DUMP_ALL_PACKETS = true;
    }

    if (sizeof(packed_conntrack_hash) != sizeof(uint64_t) or sizeof(packed_conntrack_hash) != 8) {
        logger<< log4cpp::Priority::INFO<<"Assertion about size of packed_conntrack_hash, it's "<<sizeof(packed_conntrack_hash)<<" instead 8";
        exit(1);
    }
 
#ifdef PCAP
    char errbuf[PCAP_ERRBUF_SIZE]; 
    struct pcap_pkthdr hdr;
#endif 

    logger<<log4cpp::Priority::INFO<<"Read configuration file";

    bool load_config_result = load_configuration_file();

    if (!load_config_result) {
        fprintf(stderr, "Can't open config file %s, please create it!", global_config_path.c_str());
        exit(1);
    }

    logger<< log4cpp::Priority::INFO<<"I need few seconds for collecting data, please wait. Thank you!";

    if (work_on_interfaces == "" && argc != 2) {
        fprintf(stdout, "Usage: %s \"eth0\" or \"eth0,eth1\" or specify interfaces param in config file\n", argv[0]);
        exit(1);
    }
 
    // If we found params on command line we sue it 
    if (argc >= 2 && strlen(argv[1]) > 0) {
        work_on_interfaces = argv[1];
    } 

    logger<< log4cpp::Priority::INFO<<"We selected interface:"<<work_on_interfaces;

    load_our_networks_list();

    // Setup CTRL+C handler
    signal(SIGINT, signal_handler);

#ifdef REDIS
    // Init redis connection
    if (redis_enabled) {
        if (!redis_init_connection()) {
            logger<< log4cpp::Priority::ERROR<<"Can't establish connection to the redis";
            exit(1);
        }
    }
#endif

#ifdef GEOIP
    // Init GeoIP
    if(!geoip_init()) {
        logger<< log4cpp::Priority::ERROR<<"Can't load geoip tables";
        exit(1);
    } 
#endif
    // Init previous run date
    time(&last_call_of_traffic_recalculation);

    // Run screen draw thread
    boost::thread calc_thread(calculation_thread);

    // start thread for recalculating speed in realtime
    boost::thread recalculate_speed_thread(recalculate_speed_thread_handler);

    // Run banlist cleaner thread 
    boost::thread cleanup_ban_list_thread(cleanup_ban_list);

    // pf_ring processing
    boost::thread main_packet_process_thread;

    if (enable_data_collection_from_mirror) {
        main_packet_process_thread = boost::thread(main_packet_process_task);   
    }

    boost::thread sflow_process_collector_thread; 
    if (enable_sflow_collection) {
        sflow_process_collector_thread = boost::thread(start_sflow_collection, process_packet);
    }

    if (enable_sflow_collection) {
        sflow_process_collector_thread.join();
    }

    if (enable_data_collection_from_mirror) {
        main_packet_process_thread.join();
    }

    recalculate_speed_thread.join();
    calc_thread.join();

    free_up_all_resources();
 
    return 0;
}

// Main worker thread for packet handling
void main_packet_process_task() {
    const char* device_name = work_on_interfaces.c_str();

#ifdef PCAP
    pcap_main_loop(device_name);
#endif

#ifdef PF_RING
    bool pf_ring_init_result = false;   

    if (pf_ring_zc_api_mode) {
        pf_ring_init_result = zc_main_loop((char*)device_name); 
    } else {
        if (enable_pfring_multi_channel_mode) {
            pf_ring_init_result = pf_ring_main_loop_multi_channel(device_name);
        } else {
            pf_ring_init_result = pf_ring_main_loop(device_name);
        }
    }

    if (!pf_ring_init_result) {
        // Internal error in PF_RING
        logger<< log4cpp::Priority::ERROR<<"PF_RING initilization failed, exit from programm"; 
        exit(1);
    }
#endif

}
 
void free_up_all_resources() {
#ifdef GEOIP
    // Free up geoip handle 
    GeoIP_delete(geo_ip);
#endif

    Destroy_Patricia(lookup_tree,    (void_fn_t)0);
    Destroy_Patricia(whitelist_tree, (void_fn_t)0);
}

#ifdef PF_RING 
bool pf_ring_main_loop_multi_channel(const char* dev) {
    int MAX_NUM_THREADS = 64;

    if ((threads = (struct thread_stats*)calloc(MAX_NUM_THREADS, sizeof(struct thread_stats))) == NULL) {
        logger<< log4cpp::Priority::ERROR<<"Can't allocate memory for threads structure";
        return false;
    }

    u_int32_t flags = 0;

    flags |= PF_RING_PROMISC;            /* hardcode: promisc=1 */
    flags |= PF_RING_DNA_SYMMETRIC_RSS;  /* Note that symmetric RSS is ignored by non-DNA drivers */
    flags |= PF_RING_LONG_HEADER;

    packet_direction direction = rx_only_direction;

    pfring* ring_array[MAX_NUM_RX_CHANNELS];
    
    unsigned int snaplen = 128;
    num_pfring_channels = pfring_open_multichannel(dev, snaplen, flags, ring_array);

    if (num_pfring_channels <= 0) {
        logger<< log4cpp::Priority::INFO<<"pfring_open_multichannel returned: "<<num_pfring_channels<<" and error:"<<strerror(errno);
        return false;
    }

    u_int num_cpus = sysconf( _SC_NPROCESSORS_ONLN );
    logger<< log4cpp::Priority::INFO<<"We have: "<<num_cpus<<" logical cpus in this server";    
    logger<< log4cpp::Priority::INFO<<"We have: "<<num_pfring_channels<<" channels from pf_ring NIC";

    // We should not start more processes then we have kernel cores
    //if (num_pfring_channels > num_cpus) {
    //    num_pfring_channels = num_cpus;
    //}

    for (int i = 0; i < num_pfring_channels; i++) {
        // char buf[32];
  
        threads[i].ring = ring_array[i];
        // threads[i].core_affinity = threads_core_affinity[i];

        int rc = 0;

        if  ((rc = pfring_set_direction(threads[i].ring, direction)) != 0) {
            logger<< log4cpp::Priority::INFO<<"pfring_set_direction returned: "<<rc;
        }
   
        if ((rc = pfring_set_socket_mode(threads[i].ring, recv_only_mode)) != 0) {
            logger<< log4cpp::Priority::INFO<<"pfring_set_socket_mode returned: "<<rc;
        }

        int rehash_rss = 0;

        if (rehash_rss)
            pfring_enable_rss_rehash(threads[i].ring);
  
        int poll_duration = 0; 
        if (poll_duration > 0)
            pfring_set_poll_duration(threads[i].ring, poll_duration);

        pfring_enable_ring(threads[i].ring);

        unsigned long thread_id = i;
        pthread_create(&threads[i].pd_thread, NULL, pf_ring_packet_consumer_thread, (void*)thread_id);
    }

    for(int i = 0; i < num_pfring_channels; i++) {
        pthread_join(threads[i].pd_thread, NULL);
        pfring_close(threads[i].ring);
    }

    return true;
}

void* pf_ring_packet_consumer_thread(void* _id) {
    long thread_id = (long)_id;
    int wait_for_packet = 1;

    // TODO: fix it
    bool do_shutdown = false;

    while (!do_shutdown) {
        u_char *buffer = NULL;
        struct pfring_pkthdr hdr;

        if (pfring_recv(threads[thread_id].ring, &buffer, 0, &hdr, wait_for_packet) > 0) {
            // TODO: pass (u_char*)thread_id)
            parse_packet_pf_ring(&hdr, buffer, 0);
        } else {
            if (wait_for_packet == 0) {
                usleep(1); //sched_yield();
            }
        }
   }

   return NULL;
}
#endif

#ifdef PF_RING
pthread_t *zc_threads;
pfring_zc_cluster *zc;
pfring_zc_worker *zw;
pfring_zc_queue **inzq;
pfring_zc_queue **outzq;
pfring_zc_multi_queue *outzmq; /* fanout */
pfring_zc_buffer_pool *wsp;
pfring_zc_pkt_buff **buffers;
#endif

#ifdef PF_RING
int rr = -1;

int32_t rr_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
    long num_out_queues = (long) user;

    if (++rr == num_out_queues) {
        rr = 0;
    }

    return rr;
}


int bind2core(int core_id) {
    cpu_set_t cpuset;
    int s;

    if (core_id < 0)
        return -1; 

    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if ((s = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset)) != 0) {
        logger<< log4cpp::Priority::INFO<<"Error while binding to core:"<<core_id;
        return -1; 
    } else {
        return 0;
    }
}

void *zc_packet_consumer_thread(void *_id) {
    long id = (long) _id;
    pfring_zc_pkt_buff *b = buffers[id];

    // Bind to core with thread number
    bind2core(id);

    u_int8_t wait_for_packet = 1;

    struct pfring_pkthdr zc_header;
    memset(&zc_header, 0, sizeof(zc_header));

    while (true) {
        if (pfring_zc_recv_pkt(outzq[id], &b, wait_for_packet) > 0) {
            u_char *pkt_data = pfring_zc_pkt_buff_data(b, outzq[id]);

            memset(&zc_header, 0, sizeof(zc_header));
            zc_header.len = b->len; 
            zc_header.caplen = b->len;

            pfring_parse_pkt(pkt_data, (struct pfring_pkthdr*)&zc_header, 4, 1, 0);

            parse_packet_pf_ring(&zc_header, pkt_data, 0);
        }
    }

    pfring_zc_sync_queue(outzq[id], rx_only);
        
    return NULL;
}

int max_packet_len(const char *device) { 
    int max_len;
    pfring *ring;

    ring = pfring_open(device, 1536, PF_RING_PROMISC);

    if (ring == NULL)
        return 1536;

// pfring_get_max_packet_size added in 6.0.3
#if RING_VERSION_NUM >= 0x060003
    max_len = pfring_get_max_packet_size(ring);
#else
    if (ring->dna.dna_mapped_device) {
        max_len = ring->dna.dna_dev.mem_info.rx.packet_memory_slot_len;
    } else {
        max_len = pfring_get_mtu_size(ring);
        if (max_len == 0) max_len = 9000 /* Jumbo */;
            max_len += 14 /* Eth */ + 4 /* VLAN */;
    }
#endif

    pfring_close(ring);

    return max_len;
}

#define MAX_CARD_SLOTS      32768
#define PREFETCH_BUFFERS        8
#define QUEUE_LEN            8192

bool zc_main_loop(const char* device) {
    u_int32_t cluster_id = 0;
    int bind_core = -1;
   
    u_int num_cpus = sysconf( _SC_NPROCESSORS_ONLN );
    logger<< log4cpp::Priority::INFO<<"We have: "<<num_cpus<<" logical cpus in this server";

    // TODO: add support for multiple devices!
    u_int32_t num_devices = 1;
    zc_num_threads = num_cpus - 1;   
    logger<< log4cpp::Priority::INFO<<"We will start "<<zc_num_threads<<" worker threads";
 
    u_int32_t tot_num_buffers = (num_devices * MAX_CARD_SLOTS) + (zc_num_threads * QUEUE_LEN) + zc_num_threads + PREFETCH_BUFFERS; 
 
    u_int32_t buffer_len = max_packet_len(device);   
    logger<< log4cpp::Priority::INFO<<"We got max packet len from device: "<<buffer_len; 
    logger<< log4cpp::Priority::INFO<<"We will use total number of ZC buffers: "<<tot_num_buffers;
    
    zc = pfring_zc_create_cluster(
        cluster_id, 
        buffer_len,
        0,  
        tot_num_buffers,
        numa_node_of_cpu(bind_core),
        NULL /* auto hugetlb mountpoint */ 
    );  

    if (zc == NULL) {
        logger<< log4cpp::Priority::INFO<<"pfring_zc_create_cluster error: "<<strerror(errno)<<" Please check that pf_ring.ko is loaded and hugetlb fs is mounted";
        return false; 
    }

    zc_threads  = (pthread_t *)calloc(zc_num_threads,           sizeof(pthread_t));
    buffers     = (pfring_zc_pkt_buff**)calloc(zc_num_threads,  sizeof(pfring_zc_pkt_buff *));
    inzq        = (pfring_zc_queue**)calloc(num_devices,        sizeof(pfring_zc_queue *));
    outzq       = (pfring_zc_queue**)calloc(zc_num_threads,     sizeof(pfring_zc_queue *));

    for (int i = 0; i < zc_num_threads; i++) { 
        buffers[i] = pfring_zc_get_packet_handle(zc);

        if (buffers[i] == NULL) {
            logger<< log4cpp::Priority::ERROR<<"pfring_zc_get_packet_handle failed";
            return false;
        }
    }


    for (int i = 0; i < num_devices; i++) {
        u_int32_t zc_flags = 0;
        inzq[i] = pfring_zc_open_device(zc, device, rx_only, zc_flags);

        if (inzq[i] == NULL) {
            logger<< log4cpp::Priority::ERROR<<"pfring_zc_open_device error "<<strerror(errno)<<" Please check that device is up and not already used";
            return false;
        }

#if RING_VERSION_NUM >= 0x060003
        int pf_ring_license_state = pfring_zc_check_license();

        if (!pf_ring_license_state) {
            logger<< log4cpp::Priority::ERROR<<"PF_RING ZC haven't license for device"<<device
                <<" and running in trial mode and will work only 5 minutes! Please buy license or switch to vanilla PF_RING";
        }  
#endif
    }

    for (int i = 0; i < zc_num_threads; i++) { 
        outzq[i] = pfring_zc_create_queue(zc, QUEUE_LEN);
        
        if (outzq[i] == NULL) {
            logger<< log4cpp::Priority::ERROR<<"pfring_zc_create_queue error: "<<strerror(errno);
            return false;
        }
    }
   
    wsp = pfring_zc_create_buffer_pool(zc, PREFETCH_BUFFERS);

    if (wsp == NULL) {
        logger<< log4cpp::Priority::ERROR<<"pfring_zc_create_buffer_pool error";
        return false;
    } 

    logger<< log4cpp::Priority::INFO<<"We are starting balancer with: "<<zc_num_threads<<" threads";
   
    pfring_zc_distribution_func func = rr_distribution_func;

    u_int8_t wait_for_packet = 1;
 
    // We run balancer at last thread
    int32_t bind_worker_core = 3;

    zw = pfring_zc_run_balancer(
        inzq, 
        outzq, 
        num_devices, 
        zc_num_threads, 
        wsp,
        round_robin_bursts_policy, 
        NULL /* idle callback */,
        func,
        (void *) ((long) zc_num_threads),
        !wait_for_packet, 
        bind_worker_core
    );

    if (zw == NULL) {
        logger<< log4cpp::Priority::ERROR<<"pfring_zc_run_balancer error:"<<strerror(errno);
        return false;
    }    

    for (int i = 0; i < zc_num_threads; i++) {
        pthread_create(&zc_threads[i], NULL, zc_packet_consumer_thread, (void*)(long)i);
    }

    for (int i = 0; i < zc_num_threads; i++) {
        pthread_join(zc_threads[i], NULL);
    }

    pfring_zc_kill_worker(zw);
    pfring_zc_destroy_cluster(zc);
    
    return true;
}
#endif
 
#ifdef PF_RING 
bool pf_ring_main_loop(const char* dev) {
    // We could pool device in multiple threads
    unsigned int num_threads = 1;

    bool promisc = true;
    /* This flag manages packet parser for extended_hdr */
    bool use_extended_pkt_header = true;
    bool enable_hw_timestamp = false;
    bool dont_strip_timestamps = false; 

    u_int32_t flags = 0;
    if (num_threads > 1)         flags |= PF_RING_REENTRANT;
    if (use_extended_pkt_header) flags |= PF_RING_LONG_HEADER;
    if (promisc)                 flags |= PF_RING_PROMISC;
    if (enable_hw_timestamp)     flags |= PF_RING_HW_TIMESTAMP;
    if (!dont_strip_timestamps)  flags |= PF_RING_STRIP_HW_TIMESTAMP;

    if (!we_use_pf_ring_in_kernel_parser) {
        flags |= PF_RING_DO_NOT_PARSE;
    }

    flags |= PF_RING_DNA_SYMMETRIC_RSS;  /* Note that symmetric RSS is ignored by non-DNA drivers */ 

    // use default value from pfcount.c
    unsigned int snaplen = 128;

    pf_ring_descr = pfring_open(dev, snaplen, flags); 

    if (pf_ring_descr == NULL) {
        logger<< log4cpp::Priority::INFO<<"pfring_open error: "<<strerror(errno)
            << " (pf_ring not loaded or perhaps you use quick mode and have already a socket bound to: "<<dev<< ")";
        return false;
    }


    logger<< log4cpp::Priority::INFO<<"Successully binded to: "<<dev;
    logger<< log4cpp::Priority::INFO<<"Device RX channels number: "<< pfring_get_num_rx_channels(pf_ring_descr); 

    u_int32_t version;
    // Set spplication name in /proc
    int pfring_set_application_name_result =
        pfring_set_application_name(pf_ring_descr, (char*)"fastnetmon");

    if (pfring_set_application_name_result != 0) {
        logger<< log4cpp::Priority::ERROR<<"Can't set programm name for PF_RING: pfring_set_application_name";
    }

    pfring_version(pf_ring_descr, &version);

    logger.info(
        "Using PF_RING v.%d.%d.%d",
       (version & 0xFFFF0000) >> 16, (version & 0x0000FF00) >> 8, version & 0x000000FF
    );
    
    int pfring_set_socket_mode_result =  pfring_set_socket_mode(pf_ring_descr, recv_only_mode);

    if (pfring_set_socket_mode_result != 0) {
        logger.info("pfring_set_socket_mode returned [rc=%d]\n", pfring_set_socket_mode_result);
    }  
 
    // enable ring
    if (pfring_enable_ring(pf_ring_descr) != 0) {
        logger<< log4cpp::Priority::INFO<<"Unable to enable ring :-(";
        pfring_close(pf_ring_descr);
        return false;
    }

    // Active wait wor packets. But I did not know what is mean..
    u_int8_t wait_for_packet = 1;

    pfring_loop(pf_ring_descr, parse_packet_pf_ring, (u_char*)NULL, wait_for_packet);

    return true;
}
#endif
 
#ifdef PCAP 
void pcap_main_loop(const char* dev) {
    char errbuf[PCAP_ERRBUF_SIZE];
    /* open device for reading in promiscuous mode */
    int promisc = 1;

    bpf_u_int32 maskp; /* subnet mask */
    bpf_u_int32 netp;  /* ip */ 

    logger<< log4cpp::Priority::INFO<<"Start listening on "<<dev;

    /* Get the network address and mask */
    pcap_lookupnet(dev, &netp, &maskp, errbuf);

    descr = pcap_create(dev, errbuf);

    if (descr == NULL) {
        logger<< log4cpp::Priority::ERROR<<"pcap_create was failed with error: "<<errbuf;
        exit(0);
    }

    // Setting up 1MB buffer
    int set_buffer_size_res = pcap_set_buffer_size(descr, pcap_buffer_size_mbytes * 1024 * 1024);
    if (set_buffer_size_res != 0 ) {
        if (set_buffer_size_res == PCAP_ERROR_ACTIVATED) {
            logger<< log4cpp::Priority::ERROR<<"Can't set buffer size because pcap already activated\n";
            exit(1);
        } else {
            logger<< log4cpp::Priority::ERROR<<"Can't set buffer size due to error: "<<set_buffer_size_res;
            exit(1);
        }   
    } 

    if (pcap_set_promisc(descr, promisc) != 0) {
        logger<< log4cpp::Priority::ERROR<<"Can't activate promisc mode for interface: "<<dev;
        exit(1);
    }

    if (pcap_activate(descr) != 0) {
        logger<< log4cpp::Priority::ERROR<<"Call pcap_activate was failed: "<<pcap_geterr(descr);
        exit(1);
    }

    // man pcap-linktype
    int link_layer_header_type = pcap_datalink(descr);

    if (link_layer_header_type == DLT_EN10MB) {
        DATA_SHIFT_VALUE = 14;
    } else if (link_layer_header_type == DLT_LINUX_SLL) {
        DATA_SHIFT_VALUE = 16;
    } else {
        logger<< log4cpp::Priority::INFO<<"We did not support link type:", link_layer_header_type;
        exit(0);
    }
   
    pcap_loop(descr, -1, (pcap_handler)parse_packet, NULL);
}
#endif

// For correct programm shutdown by CTRL+C
void signal_handler(int signal_number) {

#ifdef PCAP
    // Stop PCAP loop
    pcap_breakloop(descr);
#endif

#ifdef PF_RING
    pfring_breakloop(pf_ring_descr);
#endif

#ifdef REDIS
    if (redis_enabled) {
        redisFree(redis_context);
    }
#endif
    exit(1); 
}

/* Get traffic type: check it belongs to our IPs */
direction get_packet_direction(uint32_t src_ip, uint32_t dst_ip, unsigned long& subnet) {
    direction packet_direction;

    bool our_ip_is_destination = false;
    bool our_ip_is_source = false;

    prefix_t prefix_for_check_adreess;
    prefix_for_check_adreess.family = AF_INET;
    prefix_for_check_adreess.bitlen = 32;

    patricia_node_t* found_patrica_node = NULL;
    prefix_for_check_adreess.add.sin.s_addr = dst_ip;

    unsigned long destination_subnet = 0;
    found_patrica_node = patricia_search_best2(lookup_tree, &prefix_for_check_adreess, 1);

    if (found_patrica_node) {
        our_ip_is_destination = true;
        destination_subnet = found_patrica_node->prefix->add.sin.s_addr;
    }    

    found_patrica_node = NULL;
    prefix_for_check_adreess.add.sin.s_addr = src_ip;

    unsigned long source_subnet = 0;
    found_patrica_node = patricia_search_best2(lookup_tree, &prefix_for_check_adreess, 1);

    if (found_patrica_node) { 
        our_ip_is_source = true;
        source_subnet = found_patrica_node->prefix->add.sin.s_addr;
    } 

    subnet = 0;
    if (our_ip_is_source && our_ip_is_destination) {
        packet_direction = INTERNAL;
    } else if (our_ip_is_source) {
        subnet = source_subnet;
        packet_direction = OUTGOING;
    } else if (our_ip_is_destination) {
        subnet = destination_subnet;
        packet_direction = INCOMING;
    } else {
        packet_direction = OTHER;
    }

    return packet_direction;
}

unsigned int detect_attack_protocol(map_element& speed_element, direction attack_direction) {
    if (attack_direction == INCOMING) {
        return get_max_used_protocol(speed_element.tcp_in_packets, speed_element.udp_in_packets, speed_element.icmp_in_packets);
    } else {
        // OUTGOING
        return get_max_used_protocol(speed_element.tcp_out_packets, speed_element.udp_out_packets, speed_element.icmp_out_packets);    
    }
} 

unsigned int get_max_used_protocol(uint64_t tcp, uint64_t udp, uint64_t icmp) {
    unsigned int max = max(max(udp, tcp), icmp);

    if (max == tcp) {
        return IPPROTO_TCP;
    } else if (max == udp) {
        return IPPROTO_UDP;
    } else if (max == icmp) {
        return IPPROTO_ICMP;
    }

    return 0;
}

void execute_ip_ban(uint32_t client_ip, map_element speed_element, uint64_t in_pps, uint64_t out_pps, uint64_t in_bps, uint64_t out_bps, uint64_t in_flows, uint64_t out_flows, string flow_attack_details) {
    struct attack_details current_attack;
    uint64_t pps = 0;

    direction data_direction;

    if (!we_do_real_ban) {
        logger<<log4cpp::Priority::INFO<<"We do not ban: "<<convert_ip_as_uint_to_string(client_ip)<<" because ban disabled completely";
        return;
    }

    // Detect attack direction with simple heuristic 
    if (abs(int((int)in_pps - (int)out_pps)) < 1000) {
        // If difference between pps speed is so small we should do additional investigation using bandwidth speed 
        if (in_bps > out_bps) {
            data_direction = INCOMING;
            pps = in_pps;
        } else {
            data_direction = OUTGOING;
            pps = out_pps;
        } 
    } else {
        if (in_pps > out_pps) {
            data_direction = INCOMING;
            pps = in_pps;
        } else {
            data_direction = OUTGOING;
            pps = out_pps;    
        }
    }

    current_attack.attack_protocol = detect_attack_protocol(speed_element, data_direction);

    if (ban_list.count(client_ip) > 0) {
        if ( ban_list[client_ip].attack_direction != data_direction ) {
            logger<<log4cpp::Priority::INFO<<"We expected very strange situation: attack direction for "
                <<convert_ip_as_uint_to_string(client_ip)<<" was changed";

            return;
        } 

        // update attack power
        if (pps > ban_list[client_ip].max_attack_power) {
            ban_list[client_ip].max_attack_power = pps;
        }

        return;
    }

    prefix_t prefix_for_check_adreess;
    prefix_for_check_adreess.add.sin.s_addr = client_ip;
    prefix_for_check_adreess.family = AF_INET;
    prefix_for_check_adreess.bitlen = 32;

    bool in_white_list = (patricia_search_best2(whitelist_tree, &prefix_for_check_adreess, 1) != NULL);
    
    if (in_white_list) {
        return;
    }  

    string data_direction_as_string = get_direction_name(data_direction);

    logger.info("We run execute_ip_ban code with following params in_pps: %d out_pps: %d in_bps: %d out_bps: %d and we decide it's %s attack",
        in_pps, out_pps, in_bps, out_bps, data_direction_as_string.c_str());

    string client_ip_as_string = convert_ip_as_uint_to_string(client_ip);
    string pps_as_string = convert_int_to_string(pps);

    // Store ban time
    time(&current_attack.ban_timestamp); 
    // set ban time in seconds
    current_attack.ban_time = standard_ban_time;

    // Pass main information about attack
    current_attack.attack_direction = data_direction;
    current_attack.attack_power = pps;
    current_attack.max_attack_power = pps;

    current_attack.in_packets  = in_pps;
    current_attack.out_packets = out_pps;

    current_attack.in_bytes = in_bps;
    current_attack.out_bytes = out_bps;

    // pass flow information
    current_attack.in_flows = in_flows;
    current_attack.out_flows = out_flows;

    current_attack.tcp_in_packets  = speed_element.tcp_in_packets;
    current_attack.udp_in_packets  = speed_element.udp_in_packets;
    current_attack.icmp_in_packets = speed_element.icmp_in_packets;
    
    current_attack.tcp_out_packets = speed_element.tcp_out_packets;
    current_attack.udp_out_packets = speed_element.udp_out_packets;
    current_attack.icmp_out_packets = speed_element.icmp_out_packets;

    current_attack.tcp_out_bytes  = speed_element.tcp_out_bytes;
    current_attack.udp_out_bytes  = speed_element.udp_out_bytes;
    current_attack.icmp_out_bytes = speed_element.icmp_out_bytes;

    current_attack.tcp_in_bytes = speed_element.tcp_in_bytes;
    current_attack.udp_in_bytes = speed_element.udp_in_bytes;
    current_attack.icmp_in_bytes = speed_element.icmp_in_bytes;

    // Add average counters
    map_element* current_average_speed_element = &SpeedCounterAverage[client_ip];
   
    current_attack.average_in_packets = current_average_speed_element->in_packets;
    current_attack.average_in_bytes   = current_average_speed_element->in_bytes;
    current_attack.average_in_flows   = current_average_speed_element->in_flows;

    current_attack.average_out_packets = current_average_speed_element->out_packets;
    current_attack.average_out_bytes   = current_average_speed_element->out_bytes;
    current_attack.average_out_flows   = current_average_speed_element->out_flows;
    
    ban_list_mutex.lock();
    ban_list[client_ip] = current_attack;
    ban_list_mutex.unlock();

    ban_list_details_mutex.lock();
    ban_list_details[client_ip] = vector<simple_packet>();
    ban_list_details_mutex.unlock();                         

    logger<<log4cpp::Priority::INFO<<"Attack with direction: " << data_direction_as_string
        << " IP: " << client_ip_as_string << " Power: "<<pps_as_string;
    
#ifdef HWFILTER_LOCKING
    logger<<log4cpp::Priority::INFO<<"We will block traffic to/from this IP with hardware filters";
    block_all_traffic_with_82599_hardware_filtering(client_ip_as_string);
#endif

    string full_attack_description = get_attack_description(client_ip, current_attack) + flow_attack_details;
    print_attack_details_to_file(full_attack_description, client_ip_as_string, current_attack);

    if (file_exists(notify_script_path)) {
        string script_call_params = notify_script_path + " " + client_ip_as_string + " " + data_direction_as_string + " " + pps_as_string + " attack_details";
        logger<<log4cpp::Priority::INFO<<"Call script for ban client: "<<client_ip_as_string; 

        // We should execute external script in separate thread because any lag in this code will be very distructive 
        boost::thread exec_thread(exec_with_stdin_params, script_call_params, full_attack_description);
        exec_thread.detach();

        logger<<log4cpp::Priority::INFO<<"Script for ban client is finished: "<<client_ip_as_string;
    }    
}

#ifdef HWFILTER_LOCKING
void block_all_traffic_with_82599_hardware_filtering(string client_ip_as_string) {
    /* 6 - tcp, 17 - udp, 0 - other (non tcp and non udp) */
    vector<int> banned_protocols;
    banned_protocols.push_back(17);
    banned_protocols.push_back(6);
    banned_protocols.push_back(0); 
    
    int rule_number = 10;

    // Iterate over incoming and outgoing direction
    for (int rule_direction = 0; rule_direction < 2; rule_direction++) {
        for (std::vector<int>::iterator banned_protocol = banned_protocols.begin() ;
            banned_protocol != banned_protocols.end(); ++banned_protocol) {

            /* On 82599 NIC we can ban traffic using hardware filtering rules */
        
            // Difference between fie tuple and perfect filters:
            // http://www.ntop.org/products/pf_ring/hardware-packet-filtering/ 

            hw_filtering_rule rule;
            intel_82599_five_tuple_filter_hw_rule *ft_rule;

            ft_rule = &rule.rule_family.five_tuple_rule;

            memset(&rule, 0, sizeof(rule));
            rule.rule_family_type = intel_82599_five_tuple_rule;
            rule.rule_id = rule_number++;
            ft_rule->queue_id = -1; // drop traffic
            ft_rule->proto = *banned_protocol;

            string hw_filter_rule_direction = "";
            if (rule_direction == 0) {
                hw_filter_rule_direction = "outgoing";
                ft_rule->s_addr = ntohl(inet_addr(client_ip_as_string.c_str()));
            } else {
                hw_filter_rule_direction = "incoming";
                ft_rule->d_addr = ntohl(inet_addr(client_ip_as_string.c_str()));
            }

            if (pfring_add_hw_rule(pf_ring_descr, &rule) != 0) {
                logger<<log4cpp::Priority::ERROR<<"Can't add hardware filtering rule for protocol: "<<*banned_protocol<<" in direction: "<<hw_filter_rule_direction;
            }

            rule_number ++;
        }
    }
}
#endif
         
/* Thread for cleaning up ban list */
void cleanup_ban_list() {
    // Every X seconds we will run ban list cleaner thread
    int iteration_sleep_time = 600;

    logger<<log4cpp::Priority::INFO<<"Run banlist cleanup thread";

    while (true) {
        // Sleep for ten minutes
        boost::this_thread::sleep(boost::posix_time::seconds(iteration_sleep_time));

        time_t current_time;
        time(&current_time);

        map<uint32_t,banlist_item>::iterator itr = ban_list.begin();
        while (itr != ban_list.end()) {
            uint32_t client_ip = (*itr).first;

            double time_difference = difftime(current_time, ((*itr).second).ban_timestamp);
            int ban_time = ((*itr).second).ban_time;

            if (time_difference > ban_time) {
                // Cleanup all data related with this attack
                string data_direction_as_string = get_direction_name((*itr).second.attack_direction);
                string client_ip_as_string = convert_ip_as_uint_to_string(client_ip);
                string pps_as_string = convert_int_to_string((*itr).second.attack_power);

                logger<<log4cpp::Priority::INFO<<"We will unban banned IP: "<<client_ip_as_string<<
                    " because it ban time "<<ban_time<<" seconds is ended";

                ban_list_mutex.lock();
                map<uint32_t,banlist_item>::iterator itr_to_erase = itr;
                itr++;

                ban_list.erase(itr_to_erase);
                ban_list_mutex.unlock();

                if (file_exists(notify_script_path)) {
                    string script_call_params = notify_script_path + " " + client_ip_as_string + " " +
                        data_direction_as_string + " " + pps_as_string + " unban";
     
                    logger<<log4cpp::Priority::INFO<<"Call script for unban client: "<<client_ip_as_string; 

                    // We should execute external script in separate thread because any lag in this code will be very distructive 
                    boost::thread exec_thread(exec, script_call_params);
                    exec_thread.detach();

                    logger<<log4cpp::Priority::INFO<<"Script for unban client is finished: "<<client_ip_as_string;
                }
            } else {
               itr++; 
            } 
        }
    }
}

string print_time_t_in_fastnetmon_format(time_t current_time) {
    struct tm* timeinfo;
    char buffer[80];

    timeinfo = localtime (&current_time);

    strftime (buffer, sizeof(buffer), "%d_%m_%y_%H:%M:%S", timeinfo);
    puts (buffer);

    return std::string(buffer);
}

string print_ddos_attack_details() {
    stringstream output_buffer;

    for( map<uint32_t,banlist_item>::iterator ii=ban_list.begin(); ii!=ban_list.end(); ++ii) {
        uint32_t client_ip = (*ii).first; 

        string client_ip_as_string = convert_ip_as_uint_to_string(client_ip);
        string pps_as_string = convert_int_to_string(((*ii).second).attack_power);
        string max_pps_as_string = convert_int_to_string(((*ii).second).max_attack_power);
        string attack_direction = get_direction_name(((*ii).second).attack_direction);

        output_buffer<<client_ip_as_string<<"/"<<max_pps_as_string<<" pps "<<attack_direction<<" at "<<print_time_t_in_fastnetmon_format(ii->second.ban_timestamp)<<endl;

        send_attack_details(client_ip, (*ii).second);
    }


    return output_buffer.str();
}

string get_attack_description(uint32_t client_ip, attack_details& current_attack) {
    stringstream attack_description;

    attack_description
        <<"IP: "<<convert_ip_as_uint_to_string(client_ip)<<"\n"
        <<"Initial attack power: "  <<current_attack.attack_power<<" packets per second\n"
        <<"Peak attack power: "     <<current_attack.max_attack_power<< " packets per second\n"
        <<"Attack direction: "      <<get_direction_name(current_attack.attack_direction)<<"\n"
        <<"Attack protocol: "       <<get_printable_protocol_name(current_attack.attack_protocol)<<"\n"

        <<"Total incoming traffic: "      <<convert_speed_to_mbps(current_attack.in_bytes)<<" mbps\n"
        <<"Total outgoing traffic: "      <<convert_speed_to_mbps(current_attack.out_bytes)<<" mbps\n"
        <<"Total incoming pps: "          <<current_attack.in_packets<<" packets per second\n"
        <<"Total outgoing pps: "          <<current_attack.out_packets<<" packets per second\n"
        <<"Total incoming flows: "        <<current_attack.in_flows<<" flows per second\n"
        <<"Total outgoing flows: "        <<current_attack.out_flows<<" flows per second\n";

    // Add average counters 
    attack_description
        <<"Average incoming traffic: " << convert_speed_to_mbps(current_attack.average_in_bytes)  <<" mbps\n"
        <<"Average outgoing traffic: " << convert_speed_to_mbps(current_attack.average_out_bytes) <<" mbps\n"
        <<"Average incoming pps: "     << current_attack.average_in_packets                       <<" packets per second\n"
        <<"Average outgoing pps: "     << current_attack.average_out_packets                      <<" packets per second\n"
        <<"Average incoming flows: "   << current_attack.average_in_flows                         <<" flows per second\n"
        <<"Average outgoing flows: "   << current_attack.average_out_flows                        <<" flows per second\n";

    attack_description
        <<"Incoming tcp traffic: "      <<convert_speed_to_mbps(current_attack.tcp_in_bytes)<<" mbps\n"
        <<"Outgoing tcp traffic: "      <<convert_speed_to_mbps(current_attack.tcp_out_bytes)<<" mbps\n"
        <<"Incoming tcp pps: "          <<current_attack.tcp_in_packets<<" packets per second\n"
        <<"Outgoing tcp pps: "          <<current_attack.tcp_out_packets<<" packets per second\n"
        <<"Incoming udp traffic: "      <<convert_speed_to_mbps(current_attack.udp_in_bytes)<<" mbps\n"
        <<"Outgoing udp traffic: "      <<convert_speed_to_mbps(current_attack.udp_out_bytes)<<" mbps\n"
        <<"Incoming udp pps: "          <<current_attack.udp_in_packets<<" packets per second\n"
        <<"Outgoing udp pps: "          <<current_attack.udp_out_packets<<" packets per second\n"
        <<"Incoming icmp traffic: "     <<convert_speed_to_mbps(current_attack.icmp_in_bytes)<<" mbps\n"
        <<"Outgoing icmp traffic: "     <<convert_speed_to_mbps(current_attack.icmp_out_bytes)<<" mbps\n"
        <<"Incoming icmp pps: "         <<current_attack.icmp_in_packets<<" packets per second\n"
        <<"Outgoing icmp pps: "         <<current_attack.icmp_out_packets<<" packets per second\n";
 
    return attack_description.str();
}    

string get_protocol_name_by_number(unsigned int proto_number) {
    struct protoent* proto_ent = getprotobynumber( proto_number );
    string proto_name = proto_ent->p_name;
    return proto_name;
}       

void send_attack_details(uint32_t client_ip, attack_details current_attack_details) {
    string pps_as_string = convert_int_to_string(current_attack_details.attack_power);
    string attack_direction = get_direction_name(current_attack_details.attack_direction);
    string client_ip_as_string = convert_ip_as_uint_to_string(client_ip);

    // Very strange code but it work in 95% cases 
    if (ban_list_details.count( client_ip ) > 0 && ban_list_details[ client_ip ].size() == ban_details_records_count) {
        stringstream attack_details;

        attack_details<<get_attack_description(client_ip, current_attack_details)<<"\n\n";

        std::map<unsigned int, unsigned int> protocol_counter;
        for( vector<simple_packet>::iterator iii=ban_list_details[ client_ip ].begin(); iii!=ban_list_details[ client_ip ].end(); ++iii) {
            attack_details<<print_simple_packet( *iii );

            protocol_counter[ iii->protocol ]++;
        }

        std::map<unsigned int, unsigned int>::iterator max_proto = std::max_element(protocol_counter.begin(), protocol_counter.end(), protocol_counter.value_comp());
        attack_details<<"\n"<<"We got more packets ("
            <<max_proto->second
            <<" from "
            << ban_details_records_count
            <<") for protocol: "<< get_protocol_name_by_number(max_proto->first)<<"\n";
        
        logger<<log4cpp::Priority::INFO<<"Attack with direction: "<<attack_direction<<
            " IP: "<<client_ip_as_string<<" Power: "<<pps_as_string<<" traffic sample collected";

        print_attack_details_to_file(attack_details.str(), client_ip_as_string, current_attack_details);

        // Pass attack details to script
        if (file_exists(notify_script_path)) {
            logger<<log4cpp::Priority::INFO<<"Call script for notify about attack details for: "<<client_ip_as_string;

            string script_params = notify_script_path + " " + client_ip_as_string + " " + attack_direction  + " " + pps_as_string + " ban";

            // We should execute external script in separate thread because any lag in this code will be very distructive 
            boost::thread exec_with_params_thread(exec_with_stdin_params, script_params, attack_details.str());
            exec_with_params_thread.detach();

            logger<<log4cpp::Priority::INFO<<"Script for notify about attack details is finished: "<<client_ip_as_string;
        } 
        // Remove key and prevent collection new data about this attack
        ban_list_details.erase(client_ip);
    } 
}


string convert_timeval_to_date(struct timeval tv) {
    time_t nowtime = tv.tv_sec;
    struct tm *nowtm = localtime(&nowtime);
    
    char tmbuf[64];
    char buf[64];

    strftime(tmbuf, sizeof(tmbuf), "%Y-%m-%d %H:%M:%S", nowtm);

    snprintf(buf, sizeof(buf), "%s.%06ld", tmbuf, tv.tv_usec); 

    return string(buf);
}

// http://stackoverflow.com/questions/14528233/bit-masking-in-c-how-to-get-first-bit-of-a-byte
int extract_bit_value(uint8_t num, int bit) {
    if (bit > 0 && bit <= 8) {
        return ( (num >> (bit-1)) & 1 );
    } else {
        return 0;
    }
}

string print_tcp_flags(uint8_t flag_value) {
    if (flag_value == 0) {
        return "-";
    }

    // cod from pfring.h
    // (tcp->fin * TH_FIN_MULTIPLIER) + (tcp->syn * TH_SYN_MULTIPLIER) +
    // (tcp->rst * TH_RST_MULTIPLIER) + (tcp->psh * TH_PUSH_MULTIPLIER) +
    // (tcp->ack * TH_ACK_MULTIPLIER) + (tcp->urg * TH_URG_MULTIPLIER);

    /*
        // Required for decoding tcp flags
        #define TH_FIN_MULTIPLIER   0x01
        #define TH_SYN_MULTIPLIER   0x02
        #define TH_RST_MULTIPLIER   0x04
        #define TH_PUSH_MULTIPLIER  0x08
        #define TH_ACK_MULTIPLIER   0x10
        #define TH_URG_MULTIPLIER   0x20
    */

    vector<string> all_flags;

    if (extract_bit_value(flag_value, 1)) {
        all_flags.push_back("fin");
    }
    
    if (extract_bit_value(flag_value, 2)) {
        all_flags.push_back("syn");
    }   

    if (extract_bit_value(flag_value, 3)) {
        all_flags.push_back("rst");
    }   

    if (extract_bit_value(flag_value, 4)) {
        all_flags.push_back("psh");
    }   

    if (extract_bit_value(flag_value, 5)) {
        all_flags.push_back("ack");
    }    

    if (extract_bit_value(flag_value, 6)) {
        all_flags.push_back("urg");
    }   

    
    ostringstream flags_as_string;

    if (all_flags.empty()) {
        return "-";
    }

    // concatenate all vector elements with comma
    std::copy(all_flags.begin(), all_flags.end() - 1, std::ostream_iterator<string>(flags_as_string, ","));

    // add last element
    flags_as_string << all_flags.back();
    
    return flags_as_string.str();
}

#define BIG_CONSTANT(x) (x##LLU)

/*

    // calculate hash
    unsigned int seed = 11;
    uint64_t hash = MurmurHash64A(&current_packet, sizeof(current_packet), seed);

*/

// https://code.google.com/p/smhasher/source/browse/trunk/MurmurHash2.cpp
// 64-bit hash for 64-bit platforms
uint64_t MurmurHash64A ( const void * key, int len, uint64_t seed ) {
    const uint64_t m = BIG_CONSTANT(0xc6a4a7935bd1e995);
    const int r = 47;

    uint64_t h = seed ^ (len * m);

    const uint64_t * data = (const uint64_t *)key;
    const uint64_t * end = data + (len/8);

    while(data != end) {
        uint64_t k = *data++;

        k *= m; 
        k ^= k >> r; 
        k *= m; 
    
        h ^= k;
        h *= m; 
    }

    const unsigned char * data2 = (const unsigned char*)data;

    switch(len & 7) {
        case 7: h ^= uint64_t(data2[6]) << 48;
        case 6: h ^= uint64_t(data2[5]) << 40;
        case 5: h ^= uint64_t(data2[4]) << 32;
        case 4: h ^= uint64_t(data2[3]) << 24;
        case 3: h ^= uint64_t(data2[2]) << 16;
        case 2: h ^= uint64_t(data2[1]) << 8;
        case 1: h ^= uint64_t(data2[0]);
            h *= m;
    };
 
    h ^= h >> r;
    h *= m;
    h ^= h >> r;

    return h;
} 

bool is_cidr_subnet(const char* subnet) {
    boost::cmatch what;
    if (regex_match(subnet, what, regular_expression_cidr_pattern)) {
        return true;
    } else {
        return false;
    }
}


// http://www.gnu.org/software/libc/manual/html_node/Elapsed-Time.html
int timeval_subtract (struct timeval * result, struct timeval * x,  struct timeval * y) {
    /* Perform the carry for the later subtraction by updating y. */
    if (x->tv_usec < y->tv_usec) {
        int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
        y->tv_usec -= 1000000 * nsec;
        y->tv_sec += nsec;
    }

    if (x->tv_usec - y->tv_usec > 1000000) {
        int nsec = (x->tv_usec - y->tv_usec) / 1000000;
        y->tv_usec += 1000000 * nsec;
        y->tv_sec -= nsec;
    }

    /* Compute the time remaining to wait. tv_usec is certainly positive. */
    result->tv_sec = x->tv_sec - y->tv_sec;
    result->tv_usec = x->tv_usec - y->tv_usec;

    /* Return 1 if result is negative. */
    return x->tv_sec < y->tv_sec;
}

uint64_t convert_conntrack_hash_struct_to_integer(packed_conntrack_hash* struct_value) {
    uint64_t unpacked_data = 0;
    memcpy(&unpacked_data, struct_value, sizeof(uint64_t));
    return unpacked_data;
}

void convert_integer_to_conntrack_hash_struct(packed_session* packed_connection_data, packed_conntrack_hash* unpacked_data) {
    memcpy(unpacked_data, packed_connection_data, sizeof(uint64_t)); 
}

string print_flow_tracking_for_specified_protocol(contrack_map_type& protocol_map, string client_ip, direction flow_direction) {
    stringstream buffer;
    // We shoud iterate over all fields

    int printed_records = 0;
    for (contrack_map_type::iterator itr = protocol_map.begin(); itr != protocol_map.end(); ++itr) {
        // We should limit number of records in flow dump because syn flood attacks produce thounsands of lines
        if (printed_records > ban_details_records_count) {
            buffer<<"Flows are cropped due very long list\n";
            break;
        }

        uint64_t packed_connection_data = itr->first;
        packed_conntrack_hash unpacked_key_struct;
        convert_integer_to_conntrack_hash_struct(&packed_connection_data, &unpacked_key_struct);
      
        string opposite_ip_as_string = convert_ip_as_uint_to_string(unpacked_key_struct.opposite_ip);  
        if (flow_direction == INCOMING) {
            buffer<<client_ip<<":"<<unpacked_key_struct.dst_port<<" < "<<opposite_ip_as_string<<":"<<unpacked_key_struct.src_port<<" "; 
        } else if (flow_direction == OUTGOING) {
            buffer<<client_ip<<":"<<unpacked_key_struct.src_port<<" > "<<opposite_ip_as_string<<":"<<unpacked_key_struct.dst_port<<" ";
        } 
        
        buffer<<itr->second.bytes<<" bytes "<<itr->second.packets<<" packets";
        buffer<<"\n";

        printed_records++;
    } 

    return buffer.str();
}

/*
    Attack types: 
        - syn flood: one local port, multiple remote hosts (and maybe multiple remote ports) and small packet size
*/

/* Iterate over all flow tracking table */
bool process_flow_tracking_table(conntrack_main_struct& conntrack_element, string client_ip) {
    std::map <uint32_t, unsigned int>     uniq_remote_hosts_which_generate_requests_to_us;
    std::map <unsigned int, unsigned int> uniq_local_ports_which_target_of_connectiuons_from_inside;

    /* Process incoming TCP connections */
    for (contrack_map_type::iterator itr = conntrack_element.in_tcp.begin(); itr != conntrack_element.in_tcp.end(); ++itr) {
        uint64_t packed_connection_data = itr->first;
        packed_conntrack_hash unpacked_key_struct;
        convert_integer_to_conntrack_hash_struct(&packed_connection_data, &unpacked_key_struct);
        
        uniq_remote_hosts_which_generate_requests_to_us[unpacked_key_struct.opposite_ip]++;
        uniq_local_ports_which_target_of_connectiuons_from_inside[unpacked_key_struct.dst_port]++;
       
        // we can calc average packet size 
        // string opposite_ip_as_string = convert_ip_as_uint_to_string(unpacked_key_struct.opposite_ip);
        // unpacked_key_struct.src_port
        // unpacked_key_struct.dst_port
        // itr->second.packets
        // itr->second.bytes
    } 

    return true;
}

std::string print_flow_tracking_for_ip(conntrack_main_struct& conntrack_element, string client_ip) {
    stringstream buffer;

    string in_tcp = print_flow_tracking_for_specified_protocol(conntrack_element.in_tcp, client_ip, INCOMING);
    string in_udp = print_flow_tracking_for_specified_protocol(conntrack_element.in_udp, client_ip, INCOMING);

    bool we_have_incoming_flows = in_tcp.length() > 0 or in_udp.length() > 0;
    if (we_have_incoming_flows) {
        buffer<<"Incoming\n\n";
        
        if (in_tcp.length() > 0) {
            buffer<<"TCP\n"<<in_tcp<<"\n";
        }

        if (in_udp.length() > 0) {
            buffer<<"UDP\n"<<in_udp<<"\n";
        }

    }

    string out_tcp = print_flow_tracking_for_specified_protocol(conntrack_element.out_tcp, client_ip, OUTGOING);
    string out_udp = print_flow_tracking_for_specified_protocol(conntrack_element.out_udp, client_ip, OUTGOING);

    bool we_have_outgoing_flows = out_tcp.length() > 0 or out_udp.length() > 0;

    // print delimiter if we have flows in both directions
    if (we_have_incoming_flows && we_have_outgoing_flows) {
        buffer<<"\n";
    }

    if (we_have_outgoing_flows) {
        buffer<<"Outgoing\n\n";

        if (out_tcp.length() > 0 ) {
            buffer<<"TCP\n"<<out_tcp<<"\n";
        }

        if (out_udp.length() > 0) {
            buffer<<"UDP\n"<<out_udp<<"\n";
        }
    }

    return buffer.str();
}

string print_ban_thresholds() {
    stringstream output_buffer;

    output_buffer<<"Configuration params:\n";
    if (we_do_real_ban) {
        output_buffer<<"We call ban script: yes\n";
    } else {
        output_buffer<<"We call ban script: no\n";
    }

    output_buffer<<"Packets per second: ";
    if (enable_ban_for_pps) {
        output_buffer<<ban_threshold_pps;
    } else {
        output_buffer<<"disabled";
    }

    output_buffer<<"\n";

    output_buffer<<"Mbps per second: ";
    if (enable_ban_for_bandwidth) {
        output_buffer<<ban_threshold_mbps;
    } else {
        output_buffer<<"disabled";
    }

    output_buffer<<"\n";

    output_buffer<<"Flows per second: ";
    if (enable_ban_for_flows_per_second) {
        output_buffer<<ban_threshold_flows;
    } else {
        output_buffer<<"disabled";
    }

    output_buffer<<"\n";
    return output_buffer.str();
}

void print_attack_details_to_file(string details, string client_ip_as_string,  attack_details current_attack) { 
    ofstream my_attack_details_file;

    string ban_timestamp_as_string = print_time_t_in_fastnetmon_format(current_attack.ban_timestamp); 
    string attack_dump_path = attack_details_folder + "/" + client_ip_as_string + "_" + ban_timestamp_as_string;

    my_attack_details_file.open(attack_dump_path.c_str(), ios::app);

    if (my_attack_details_file.is_open()) {
        my_attack_details_file << details << "\n\n"; 
        my_attack_details_file.close();
    } else {
        logger<<log4cpp::Priority::ERROR<<"Can't print attack details to file";
    }    
}

#ifdef PF_RING  
string get_pf_ring_stats() {
    std::stringstream output_buffer;

    if (pf_ring_zc_api_mode) {
        pfring_zc_stat stats;
        // We have elements in insq for every hardware device! We shoulw add ability to configure ot
        int stats_res = pfring_zc_stats(inzq[0], &stats);
    
        if (stats_res) {
            logger<<log4cpp::Priority::ERROR<<"Can't get PF_RING ZC stats for in queue";
        } else {
            double dropped_percent = 0;

            if (stats.recv + stats.sent > 0) {    
                dropped_percent = (double)stats.drop / ((double)stats.recv + (double)stats.sent) * 100;
            }

            output_buffer<<"\n";
            output_buffer<<"PF_RING ZC in queue statistics\n";
            output_buffer<<"Received:\t"<<stats.recv<<"\n";
            output_buffer<<"Sent:\t\t"<<stats.sent<<"\n";
            output_buffer<<"Dropped:\t"<<stats.drop<<"\n";
            output_buffer<<"Dropped:\t"<<std::fixed << std::setprecision(2)<<dropped_percent<<" %\n";
        }

        output_buffer<<"\n";
        output_buffer<<"PF_RING ZC out queue statistics\n";

        u_int64_t total_recv = 0;
        u_int64_t total_sent = 0;
        u_int64_t total_drop = 0;
        for (int i = 0; i < zc_num_threads; i++) {
            pfring_zc_stat outq_stats;

            int outq_stats_res = pfring_zc_stats(outzq[0], &outq_stats);
            if (stats_res) {
                logger<<log4cpp::Priority::ERROR<<"Can't get PF_RING ZC stats for out queue";
            } else {
                total_recv += outq_stats.recv;
                total_sent += outq_stats.sent;
                total_drop += outq_stats.drop;
            }
        }

        double total_drop_percent = 0;

        if (total_recv + total_sent > 0) {
            total_drop_percent = (double)total_drop / ((double)total_recv + (double)total_sent) * 100;
        }

        output_buffer<<"Received:\t"<<total_recv<<"\n";
        output_buffer<<"Sent:\t\t"<<total_sent<<"\n";
        output_buffer<<"Dropped:\t"<<total_drop<<"\n";
        output_buffer<<"Dropped:\t"<<std::fixed << std::setprecision(2)<<total_drop_percent<<" %\n";
    }
    
    // Getting stats for multi channel mode is so complex task
    if (!enable_pfring_multi_channel_mode && !pf_ring_zc_api_mode) {
        pfring_stat pfring_status_data;
             
        if (pfring_stats(pf_ring_descr, &pfring_status_data) >= 0) { 
            char stats_buffer[256];
            double packets_dropped_percent = 0; 

            if (pfring_status_data.recv > 0) { 
                packets_dropped_percent = (double)pfring_status_data.drop / pfring_status_data.recv * 100; 
            }    

            sprintf(
                stats_buffer,
                "Packets received:\t%lu\n"
                "Packets dropped:\t%lu\n"
                "Packets dropped:\t%.1f %%\n",
                (long unsigned int) pfring_status_data.recv,
                (long unsigned int) pfring_status_data.drop,
                packets_dropped_percent
            );   
            output_buffer<<stats_buffer;
        } else {
            logger<< log4cpp::Priority::ERROR<<"Can't get PF_RING stats";
        }    
    }    
    
    return output_buffer.str();
}
#endif 

#ifdef PCAP
string get_pcap_stats() {
    stringstream output_buffer;

    struct pcap_stat current_pcap_stats;
    if (pcap_stats(descr, &current_pcap_stats) == 0) { 
        output_buffer<<"PCAP statistics"<<endl<<"Received packets: "<<current_pcap_stats.ps_recv<<endl
            <<"Dropped packets: "<<current_pcap_stats.ps_drop
            <<" ("<<int((double)current_pcap_stats.ps_drop/current_pcap_stats.ps_recv*100)<<"%)"<<endl
             <<"Dropped by driver or interface: "<<current_pcap_stats.ps_ifdrop<<endl;
    }    

    return output_buffer.str();   
}
#endif

