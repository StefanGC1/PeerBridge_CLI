#include "Utils.hpp"
#include "NetworkConfigManager.hpp"
#include "Logger.hpp"

#pragma comment(lib, "iphlpapi.lib")

NetworkConfigManager::SetupConfig NetworkConfigManager::SetupConfig::loadConfig()
{
    // TODO: Later, set GUID here and send it to tun->initialize
    SetupConfig cfg{
        "10.0.0.",  // IP-SPACE
        {}};        // GUID
    return cfg;
}

NetworkConfigManager::NetworkConfigManager() : setupConfig{SetupConfig::loadConfig()}
{}

bool NetworkConfigManager::configureInterface(const ConnectionConfig& connectionConfig)
{
    routeApproach = RouteConfigApproach::GENERIC_ROUTE;

    bool configurationSuccess;
    bool routeConfigSuccess = setupRouting(connectionConfig);
    if (!routeConfigSuccess)
    {
        SYSTEM_LOG_ERROR("[Network Config Manager] Interface configuration failed, removing any routes that succeded");
        removeRouting(connectionConfig.peerVirtualIp);
        return false;
    }
    setupFirewall();
    SYSTEM_LOG_INFO("[Network Config Manager] Interface configuration successful");
    return true;
}

bool NetworkConfigManager::setupRouting(const ConnectionConfig& connectionConfig)
{
    // TODO: Will have check for peerInfo in IPCServer on data, maybe add check here as well later.
    
    std::string networkAddr = setupConfig.IP_SPACE + std::to_string(NetworkConstants::BASE_IP_INDEX);
    std::string selfVirtualIp = setupConfig.IP_SPACE + std::to_string(connectionConfig.selfIndex);
    std::string netmask = NetworkConstants::NET_MASK;

    // Count mask bits for CIDR notation
    uint32_t mask = utils::ipToUint32(NetworkConstants::NET_MASK);
    int maskBits = __builtin_popcount(mask);

    SYSTEM_LOG_INFO("[Network Config Manager] Setting up routing on private IP Space: {}", networkAddr);
    SYSTEM_LOG_INFO("[Netowrk Config Manager] Setting self (static) ip as: {}", selfVirtualIp);
    SYSTEM_LOG_INFO("[Network Config Manager] Setting up routing on subnet: {}, with bits masked: {}", netmask, maskBits);

    std::ostringstream command;
    command << "netsh interface ip set address \"" << narrowAlias 
            << "\" static " << selfVirtualIp << " " << netmask;
    
    if (!executeNetshCommand(command.str()))
    {
        SYSTEM_LOG_ERROR("[Network Config Manager] Failed to set up self ip, cancelling connection");
        routeApproach = RouteConfigApproach::FAILED;
        return false;
    }

    // Approach 1: Add route using netsh with CIDR Notation
    command.str("");
    command << "netsh interface ipv4 add route " 
            << networkAddr << "/" << maskBits
            << " \"" << narrowAlias << "\""
            << " metric=1";
    
    bool success = executeNetshCommand(command.str());

    if (!success)
    {
        SYSTEM_LOG_WARNING("[Network Config Manager] Second route command failed, trying to add direct routes...");
        routeApproach = RouteConfigApproach::FALLBACK_ROUTE_ALL;
        
        // Approach 2: Try adding a specific route to peer IP
        // This ensures at least basic connectivity even if subnet routing fails
        command.str("");
        
        // TODO: REFACTORIZE FOR *1 FOR MULTIPLE PEERS
        // Get peer IP
        std::string peerIP = connectionConfig.peerVirtualIp;
        
        command << "netsh interface ipv4 add route " 
                << peerIP << "/32"
                << " \"" << narrowAlias << "\""
                << " metric=1";
        success = executeNetshCommand(command.str());

        if (!success)
        {
            SYSTEM_LOG_WARNING("[Network Config Manager] Failed to add route for virtual network, connection may be limited");
            routeApproach = RouteConfigApproach::FAILED;
        }
    }
    
    // Enable forwarding on the interface
    command.str("");
    command << "netsh interface ipv4 set interface \"" << narrowAlias << "\" forwarding=enabled metric=1";
    
    if (!executeNetshCommand(command.str()))
    {
        SYSTEM_LOG_ERROR("[Network Config Manager] Failed to enable forwarding on interface");
        return false;
    }

    // Multicast route, for discovery
    command.str("");
    command << "netsh interface ipv4 add route "
            << "prefix=" << NetworkConstants::MULTICAST_SUBNET_RANGE
            << " interface=\"" << narrowAlias << "\" "
            << "metric=1";
    if (!executeNetshCommand(command.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to add route for multicast traffic. Route may already exist, or discovery may be limited.");
    }
    
    SYSTEM_LOG_INFO("[Network Config Manager] Routing configured for virtual network");
    return true;
}

void NetworkConfigManager::setupFirewall()
{
    SYSTEM_LOG_INFO("[Network Config Manager] Setting up firewall rules");
    // Add firewall rule to allow traffic on the virtual network
    std::ostringstream firewallCmd;
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"PeerBridge IN\" "
                << "dir=in "
                << "action=allow "
                << "protocol=any "
                << "remoteip=10.0.0.0/24";
    
    // Execute firewall command (using our TUN interface's command helper)
    if (!executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to add inbound firewall rule. Connectivity may be limited.");
    }
    
    // Add outbound rule too
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"PeerBridge OUT\" "
                << "dir=out "
                << "action=allow "
                << "protocol=any "
                << "remoteip=10.0.0.0/24";
    
    if (!executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to add outbound firewall rule. Connectivity may be limited.");
    }
    
    // Add specific rule for ICMP (ping)
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"PeerBridge ICMP\" "
                << "dir=in "
                << "action=allow "
                << "protocol=icmpv4 "
                << "remoteip=10.0.0.0/24";
    
    if (!executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to add ICMP firewall rule. Ping may not work.");
    }
    
    // Enable File and Printer Sharing (needed for some network discovery protocols)
    // TODO: Investigate if it's a good idea to have this
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall set rule "
                << "group=\"File and Printer Sharing\" "
                << "new enable=Yes";
    
    if (!executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to enable File and Printer Sharing. Network discovery may be limited.");
    }

    // Add specific rule for IGMP (inbound)
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"PeerBridge IGMP IN\" "
                << "dir=in "
                << "action=allow "
                << "protocol=2 "
                << "remoteip=10.0.0.0/24";

    if (!executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to add inbound IGMP firewall rule. Multicast may not work.");
    }

    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"PeerBridge IGMP OUT\" "
                << "dir=out "
                << "action=allow "
                << "protocol=2 "
                << "remoteip=10.0.0.0/24";
    if (!executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to add outbound IGMP firewall rule. Multicast may not work.");
    }

    std::ostringstream netCategoryCmd;
    netCategoryCmd << "powershell -Command \"Set-NetConnectionProfile -InterfaceAlias "
                   << "'" << narrowAlias << "'"
                   <<" -NetworkCategory Private\"";

    if (!executeNetshCommand(netCategoryCmd.str())) {
        SYSTEM_LOG_WARNING(
            "[Network Config Manager] Failed to set network category to Private or adapter is already set to Private. LAN functionality may be limited");
    }
}

// TODO: Consider using ConnectionConfig
void NetworkConfigManager::resetInterfaceConfiguration(const std::string& peerVirtualIp)
{
    bool success = removeRouting(peerVirtualIp);
    if (!success)
        SYSTEM_LOG_INFO("[Network Config Manager] Failed to remove routing");
    removeFirewall();
    
}

// TODO: Consider using ConnectionConfig
bool NetworkConfigManager::removeRouting(const std::string& peerVirtualIp)
{
    std::string networkAddr = setupConfig.IP_SPACE + std::to_string(NetworkConstants::BASE_IP_INDEX);
    std::string netmask = NetworkConstants::NET_MASK;

    // Count mask bits for CIDR notation
    uint32_t mask = utils::ipToUint32(NetworkConstants::NET_MASK);
    uint32_t tempMask = mask;
    int maskBits = 0;
    while (tempMask) {
        maskBits += (tempMask & 1);
        tempMask >>= 1;
    }

    SYSTEM_LOG_INFO("[Network Config Manager] Removing routing on private IP Space: {}", networkAddr);
    SYSTEM_LOG_INFO("[Netowrk Config Manager] Removing self (static) ip");
    SYSTEM_LOG_INFO("[Network Config Manager] Removing routing on subnet: {}, with bits masked: {}", netmask, maskBits);

    bool success = true;

    std::ostringstream command;
    switch (routeApproach)
    {
        case RouteConfigApproach::GENERIC_ROUTE:
        {
            command << "netsh interface ipv4 delete route " 
            << networkAddr << "/" << maskBits
            << " \"" << narrowAlias << "\"";
            if (!(success = executeNetshCommand(command.str())))
                SYSTEM_LOG_INFO("[Network Config Manager] Failed to remove generic route");
            break;
        }
        case RouteConfigApproach::FALLBACK_ROUTE_ALL:
        {
            // TODO: TO BE MODIFIED FOR *1
            command << "netsh interface ipv4 delete route " 
                << peerVirtualIp << "/32"
                << " \"" << narrowAlias << "\"";
            if (!(success = executeNetshCommand(command.str())))
                SYSTEM_LOG_INFO("[Network Config Manager] Failed to remove per-peer specific routes");
            break;
        }
        case RouteConfigApproach::FAILED:
        default:
            break;
    }

    //netsh interface ip set address "wintun-mesh" dhcp
    command.str("");
    command << "netsh interface ip set address "
            << " \"" << narrowAlias << "\" "
            << "dhcp";
    
    if (!(success = executeNetshCommand(command.str())))
        SYSTEM_LOG_INFO("[Network Config Manager] Failed to remove self (static) routing");
    
    command.str("");
    command << "netsh interface ipv4 delete route "
            << "prefix= " << NetworkConstants::MULTICAST_SUBNET_RANGE
            << " interface =\"" << narrowAlias << "\"";

    if (!(success = executeNetshCommand(command.str())))
        SYSTEM_LOG_INFO("[Network Config Manager] Failed to remove multicast routing");
    
    command.str("");
    command << "netsh interface ipv4 set interface \"" << narrowAlias << "\" forwarding=disabled";

    if (!(success = executeNetshCommand(command.str())))
        SYSTEM_LOG_INFO("[Network Config Manager] Failed to remove multicast routing");

    return success;
}

void NetworkConfigManager::removeFirewall()
{
    SYSTEM_LOG_INFO("[Network Config Manager] Setting up firewall rules");

    std::ostringstream firewallCmd;
    firewallCmd << "netsh advfirewall firewall delete rule "
                << "name=\"PeerBridge IN\"";

    if (!executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to remove inbound firewall rule");
    }

    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall delete rule "
                << "name=\"PeerBridge OUT\"";
    
    if (!executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to remove outbound firewall rule");
    }

    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall delete rule "
                << "name=\"PeerBridge ICMP\"";
    
    if (!executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to remove ICMP firewall rule");
    }

    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall delete rule "
                << "name=\"PeerBridge IGMP IN\"";

    if (!executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to add inbound IGMP firewall rule.");
    }

    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall delete rule "
                << "name=\"PeerBridge IGMP OUT\"";

    if (!executeNetshCommand(firewallCmd.str())) {
        SYSTEM_LOG_WARNING("[Network Config Manager] Failed to add outbound IGMP firewall rule");
    }
}

bool NetworkConfigManager::executeNetshCommand(const std::string& command) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::string fullCommand = "cmd /c " + command;
    
    SYSTEM_LOG_INFO("[Netsh] Executing: {}", fullCommand);
    
    // Convert to wide string for CreateProcessW
    int size = MultiByteToWideChar(CP_UTF8, 0, fullCommand.c_str(), -1, NULL, 0);
    std::vector<wchar_t> wideCommand(size);
    MultiByteToWideChar(CP_UTF8, 0, fullCommand.c_str(), -1, wideCommand.data(), size);
    
    // Create the process
    if (!CreateProcessW(NULL, wideCommand.data(), NULL, NULL, FALSE, 
                      CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        SYSTEM_LOG_WARNING("[Netsh] Failed to execute command: {}", command);
        SYSTEM_LOG_WARNING("[Netsh] Windows error: {}", GetLastError());
        return false;
    }
    
    // Wait for the process to finish
    WaitForSingleObject(pi.hProcess, INFINITE);
    
    // Get the exit code
    DWORD exitCode;
    if (!GetExitCodeProcess(pi.hProcess, &exitCode)) {
        SYSTEM_LOG_WARNING("[Netsh] Failed to get process exit code");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return false;
    }
    
    // Clean up handles
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    if (exitCode != 0) {
        SYSTEM_LOG_WARNING("[Netsh] Command failed with exit code: {}", exitCode);
        SYSTEM_LOG_WARNING("[Netsh] Windows error: {}", GetLastError());
    }
    
    return exitCode == 0;
}

void NetworkConfigManager::setNarrowAlias(const std::string& nAlias)
{
    narrowAlias = nAlias;
}