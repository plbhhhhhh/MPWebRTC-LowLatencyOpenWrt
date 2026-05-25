/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef PACKETSHELL_HH
#define PACKETSHELL_HH

#include <memory>
#include <string>
#include <termios.h>

#include "netdevice.hh"
#include "nat.hh"
#include "util.hh"
#include "get_address.hh"
#include "address.hh"
#include "dns_proxy.hh"
#include "json.hh"

using json = nlohmann::json;

/* Linux IFNAMSIZ is 16 (incl. NUL); keep names <= 15 chars. Prefix uses PID so
 * multiple mpshell instances on one host do not collide on tun names. */
class PacketShell
{
private:
    std::vector<std::pair<Address, Address>> egress_ingress;
    Address nameserver_;
    std::vector<TunDevice> egress_tuns_;
    std::unique_ptr<DNSProxy> dns_outside_;
    std::vector<std::unique_ptr<NAT>> nats_;

    using Pipe = std::pair<FileDescriptor, FileDescriptor>;
    std::vector<Pipe> pipes_;
    std::vector<ChildProcess> child_processes_;

    std::string egress_prefix_;
    std::string ingress_prefix_;

public:
    explicit PacketShell( int if_num );

    void start_uplink( const std::string & shell_prefix,
                       char ** const user_environment,
                       const std::vector<uint64_t> & delays,
                       const std::vector<std::string> & uplinks,
                       const std::vector<json> & queue_params,
                       const std::string & log_file,
                       const std::vector< std::string > & command,
                       int pipefd[2] );

    void start_downlink( const std::vector<uint64_t> & delays,
                         const std::vector<std::string> & downlinks,
                         const std::vector<json> & queue_params,
                         const std::string & log_file);

    int wait_on_processes( std::vector<ChildProcess> && process_vector );
    int wait_for_exit() { return wait_on_processes( std::move( child_processes_ ) ); }

    PacketShell( const PacketShell & other ) = delete;
    PacketShell & operator=( const PacketShell & other ) = delete;
};

#endif
